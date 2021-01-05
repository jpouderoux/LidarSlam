//==============================================================================
// Copyright 2018-2020 Kitware, Inc., Kitware SAS
// Author: Guilbert Pierre (Kitware SAS)
//         Cadart Nicolas (Kitware SAS)
// Creation date: 2018-03-27
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//==============================================================================

// This slam algorithm is inspired by the LOAM algorithm:
// J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
// Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

// The algorithm is composed of three sequential steps:
//
// - Keypoints extraction: this step consists of extracting keypoints over
// the points clouds. To do that, the laser lines / scans are treated independently.
// The laser lines are projected onto the XY plane and are rescaled depending on
// their vertical angle. Then we compute their curvature and create two classes of
// keypoints. The edges keypoints which correspond to points with a high curvature
// and planar points which correspond to points with a low curvature.
//
// - Ego-Motion: this step consists of recovering the motion of the lidar
// sensor between two frames (two sweeps). The motion is modelized by a constant
// velocity and angular velocity between two frames (i.e null acceleration).
// Hence, we can parameterize the motion by a rotation and translation per sweep / frame
// and interpolate the transformation inside a frame using the timestamp of the points.
// Since the points clouds generated by a lidar are sparse we can't design a
// pairwise match between keypoints of two successive frames. Hence, we decided to use
// a closest-point matching between the keypoints of the current frame
// and the geometric features derived from the keypoints of the previous frame.
// The geometric features are lines or planes and are computed using the edges
// and planar keypoints of the previous frame. Once the matching is done, a keypoint
// of the current frame is matched with a plane / line (depending of the
// nature of the keypoint) from the previous frame. Then, we recover R and T by
// minimizing the function f(R, T) = sum(d(point, line)^2) + sum(d(point, plane)^2).
// Which can be writen f(R, T) = sum((R*X+T-P).t*A*(R*X+T-P)) where:
// - X is a keypoint of the current frame
// - P is a point of the corresponding line / plane
// - A = (n*n.t) with n being the normal of the plane
// - A = (I - n*n.t).t * (I - n*n.t) with n being a director vector of the line
// Since the function f(R, T) is a non-linear mean square error function
// we decided to use the Levenberg-Marquardt algorithm to recover its argmin.
//
// - Localization: This step consists of refining the motion recovered in the Ego-Motion
// step and to add the new frame in the environment map. Thanks to the ego-motion
// recovered at the previous step it is now possible to estimate the new position of
// the sensor in the map. We use this estimation as an initial point (R0, T0) and we
// perform an optimization again using the keypoints of the current frame and the matched
// keypoints of the map (and not only the previous frame this time!). Once the position in the
// map has been refined from the first estimation it is then possible to update the map by
// adding the keypoints of the current frame into the map.
//
// In the following programs, three 3D coordinates system are used :
// - LIDAR {L} : attached to the geometric center of the LiDAR sensor. The
//   coordinates of the received pointclouds are expressed in this system.
//   LIDAR is rigidly linked (static transform) to BASE.
// - BASE  {B} : attached to the origin of the moving body (e.g. vehicle). We
//   are generally interested in tracking an other point of the moving body than
//   the LiDAR's (for example, we prefer to track the GPS antenna pose).
// - WORLD {W} : The world coordinate system {W} coincides with BASE at the
//   initial position. The output trajectory describes BASE origin in WORLD.

#pragma once

#include "LidarSlam/Utilities.h"
#include "LidarSlam/Transform.h"
#include "LidarSlam/LidarPoint.h"
#include "LidarSlam/Enums.h"
#include "LidarSlam/SpinningSensorKeypointExtractor.h"
#include "LidarSlam/KDTreePCLAdaptor.h"
#include "LidarSlam/KeypointsRegistration.h"
#include "LidarSlam/MotionModel.h"
#include "LidarSlam/RollingGrid.h"
#include "LidarSlam/PointCloudStorage.h"

#include <Eigen/Geometry>

#include <deque>

#define SetMacro(name,type) void Set##name (type _arg) { name = _arg; }
#define GetMacro(name,type) type Get##name () const { return name; }

namespace LidarSlam
{

class Slam
{
public:
  // Needed as Slam has fixed size Eigen vectors as members
  // http://eigen.tuxfamily.org/dox-devel/group__TopicStructHavingEigenMembers.html
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

  // Usefull types
  using Point = LidarPoint;
  using PointCloud = pcl::PointCloud<Point>;
  using KDTree = KDTreePCLAdaptor<Point>;

  // Initialization
  Slam();
  void Reset(bool resetLog = true);

  // ---------------------------------------------------------------------------
  //   Main SLAM use
  // ---------------------------------------------------------------------------

  // Add a new frame to process to the slam algorithm
  // From this frame; keypoints will be computed and extracted
  // in order to recover the ego-motion of the lidar sensor
  // and to update the map using keypoints and ego-motion
  void AddFrame(const PointCloud::Ptr& pc);

  // Get the computed world transform so far (current BASE pose in WORLD coordinates)
  Transform GetWorldTransform() const;
  // Get the computed world transform so far, but compensating SLAM computation duration latency.
  Transform GetLatencyCompensatedWorldTransform() const;
  // Get the covariance of the last localization step (registering the current frame to the last map)
  // DoF order : X, Y, Z, rX, rY, rZ
  std::array<double, 36> GetTransformCovariance() const;

  // Get the whole trajectory and covariances of each step (aggregated WorldTransforms and TransformCovariances).
  // (buffer of temporal length LoggingTimeout)
  std::vector<Transform> GetTrajectory() const;
  std::vector<std::array<double, 36>> GetCovariances() const;

  // Get keypoints maps
  PointCloud::Ptr GetEdgesMap() const;
  PointCloud::Ptr GetPlanarsMap() const;
  PointCloud::Ptr GetBlobsMap() const;

  // Get extracted keypoints from current frame.
  // If worldCoordinates=false, it returns raw keypoints in BASE coordinates,
  // without undistortion. If worldCoordinates=true, it returns undistorted
  // keypoints in WORLD coordinates.
  PointCloud::Ptr GetEdgesKeypoints(bool worldCoordinates = true) const;
  PointCloud::Ptr GetPlanarsKeypoints(bool worldCoordinates = true) const;
  PointCloud::Ptr GetBlobsKeypoints(bool worldCoordinates = true) const;

  // Get current frame
  PointCloud::Ptr GetOutputFrame();

  // Get current number of frames already processed
  GetMacro(NbrFrameProcessed, unsigned int)

  // Get general information about ICP and optimization
  std::unordered_map<std::string, double> GetDebugInformation() const;
  // Get information for each keypoint of the current frame (used/rejected keypoints, ...)
  std::unordered_map<std::string, std::vector<double>> GetDebugArray() const;

  // Run pose graph optimization using GPS trajectory to improve SLAM maps and trajectory.
  // Each GPS position must have an associated precision covariance.
  // TODO : run that in a separated thread.
  void RunPoseGraphOptimization(const std::vector<Transform>& gpsPositions,
                                const std::vector<std::array<double, 9>>& gpsCovariances,
                                Eigen::Isometry3d& gpsToSensorOffset,
                                const std::string& g2oFileName = "");

  // Set world transform with an initial guess (usually from GPS after calibration).
  void SetWorldTransformFromGuess(const Transform& poseGuess);

  // Save keypoints maps to disk for later use
  void SaveMapsToPCD(const std::string& filePrefix, PCDFormat pcdFormat = PCDFormat::BINARY_COMPRESSED) const;

  // Load keypoints maps from disk (and reset SLAM maps)
  void LoadMapsFromPCD(const std::string& filePrefix, bool resetMaps = true);

  // ---------------------------------------------------------------------------
  //   General parameters
  // ---------------------------------------------------------------------------

  GetMacro(NbThreads, int)
  void SetNbThreads(int n) { this->NbThreads = n; this->KeyPointsExtractor->SetNbThreads(n); }

  SetMacro(Verbosity, int)
  GetMacro(Verbosity, int)

  GetMacro(FastSlam, bool)
  SetMacro(FastSlam, bool)

  SetMacro(EgoMotion, EgoMotionMode)
  GetMacro(EgoMotion, EgoMotionMode)

  SetMacro(Undistortion, UndistortionMode)
  GetMacro(Undistortion, UndistortionMode)

  SetMacro(LoggingTimeout, double)
  GetMacro(LoggingTimeout, double)

  SetMacro(LoggingStorage, PointCloudStorageType)
  GetMacro(LoggingStorage, PointCloudStorageType)

  SetMacro(UpdateMap, bool)
  GetMacro(UpdateMap, bool)

  // ---------------------------------------------------------------------------
  //   Coordinates systems parameters
  // ---------------------------------------------------------------------------

  SetMacro(BaseToLidarOffset, Eigen::Isometry3d const&)
  GetMacro(BaseToLidarOffset, Eigen::Isometry3d)

  SetMacro(BaseFrameId, std::string const&)
  GetMacro(BaseFrameId, std::string)

  SetMacro(WorldFrameId, std::string const&)
  GetMacro(WorldFrameId, std::string)

  // ---------------------------------------------------------------------------
  //   Optimization parameters
  // ---------------------------------------------------------------------------

  GetMacro(MaxDistanceForICPMatching, double)
  SetMacro(MaxDistanceForICPMatching, double)

  // Get/Set EgoMotion
  GetMacro(EgoMotionLMMaxIter, unsigned int)
  SetMacro(EgoMotionLMMaxIter, unsigned int)

  GetMacro(EgoMotionICPMaxIter, unsigned int)
  SetMacro(EgoMotionICPMaxIter, unsigned int)

  GetMacro(EgoMotionLineDistanceNbrNeighbors, unsigned int)
  SetMacro(EgoMotionLineDistanceNbrNeighbors, unsigned int)

  GetMacro(EgoMotionMinimumLineNeighborRejection, unsigned int)
  SetMacro(EgoMotionMinimumLineNeighborRejection, unsigned int)

  GetMacro(EgoMotionLineDistancefactor, double)
  SetMacro(EgoMotionLineDistancefactor, double)

  GetMacro(EgoMotionPlaneDistanceNbrNeighbors, unsigned int)
  SetMacro(EgoMotionPlaneDistanceNbrNeighbors, unsigned int)

  GetMacro(EgoMotionPlaneDistancefactor1, double)
  SetMacro(EgoMotionPlaneDistancefactor1, double)

  GetMacro(EgoMotionPlaneDistancefactor2, double)
  SetMacro(EgoMotionPlaneDistancefactor2, double)

  GetMacro(EgoMotionMaxLineDistance, double)
  SetMacro(EgoMotionMaxLineDistance, double)

  GetMacro(EgoMotionMaxPlaneDistance, double)
  SetMacro(EgoMotionMaxPlaneDistance, double)

  GetMacro(EgoMotionInitLossScale, double)
  SetMacro(EgoMotionInitLossScale, double)

  GetMacro(EgoMotionFinalLossScale, double)
  SetMacro(EgoMotionFinalLossScale, double)

  // Get/Set Localization
  GetMacro(LocalizationLMMaxIter, unsigned int)
  SetMacro(LocalizationLMMaxIter, unsigned int)

  GetMacro(LocalizationICPMaxIter, unsigned int)
  SetMacro(LocalizationICPMaxIter, unsigned int)

  GetMacro(LocalizationLineDistanceNbrNeighbors, unsigned int)
  SetMacro(LocalizationLineDistanceNbrNeighbors, unsigned int)

  GetMacro(LocalizationMinimumLineNeighborRejection, unsigned int)
  SetMacro(LocalizationMinimumLineNeighborRejection, unsigned int)

  GetMacro(LocalizationLineDistancefactor, double)
  SetMacro(LocalizationLineDistancefactor, double)

  GetMacro(LocalizationPlaneDistanceNbrNeighbors, unsigned int)
  SetMacro(LocalizationPlaneDistanceNbrNeighbors, unsigned int)

  GetMacro(LocalizationPlaneDistancefactor1, double)
  SetMacro(LocalizationPlaneDistancefactor1, double)

  GetMacro(LocalizationPlaneDistancefactor2, double)
  SetMacro(LocalizationPlaneDistancefactor2, double)

  GetMacro(LocalizationMaxLineDistance, double)
  SetMacro(LocalizationMaxLineDistance, double)

  GetMacro(LocalizationMaxPlaneDistance, double)
  SetMacro(LocalizationMaxPlaneDistance, double)

  GetMacro(LocalizationInitLossScale, double)
  SetMacro(LocalizationInitLossScale, double)

  GetMacro(LocalizationFinalLossScale, double)
  SetMacro(LocalizationFinalLossScale, double)

  // ---------------------------------------------------------------------------
  //   Rolling grid parameters and Keypoints extractor
  // ---------------------------------------------------------------------------

  // Set RollingGrid Parameters
  void ClearMaps();
  void SetVoxelGridLeafSizeEdges(double size);
  void SetVoxelGridLeafSizePlanes(double size);
  void SetVoxelGridLeafSizeBlobs(double size);
  void SetVoxelGridSize(int size);
  void SetVoxelGridResolution(double resolution);

  void SetKeyPointsExtractor(std::shared_ptr<SpinningSensorKeypointExtractor> extractor) { this->KeyPointsExtractor = extractor; }
  std::shared_ptr<SpinningSensorKeypointExtractor> GetKeyPointsExtractor() const { return this->KeyPointsExtractor; }

private:

  // ---------------------------------------------------------------------------
  //   General stuff and flags
  // ---------------------------------------------------------------------------

  // Max number of threads to use for parallel processing
  int NbThreads = 1;

  // If set to true the "Localization" step planars keypoints used
  // will be the same than the "EgoMotion" step ones. If set to false
  // all points that are not set to invalid will be used
  // as "Localization" step planars points.
  bool FastSlam = true;

  // How to estimate Ego-Motion (approximate relative motion since last frame).
  // The ego-motion step aims to give a fast and approximate initialization of
  // new frame world pose to ensure faster and more precise convergence in
  // Localization step.
  EgoMotionMode EgoMotion = EgoMotionMode::MOTION_EXTRAPOLATION;

  // How the algorithm should undistort the lidar scans.
  // The undistortion should improve the accuracy, but the computation speed
  // may decrease, and the result might be unstable in difficult situations.
  UndistortionMode Undistortion = UndistortionMode::APPROXIMATED;

  // Indicate verbosity level to display more or less information :
  // 0: print errors, warnings or one time info
  // 1: 0 + frame number, total frame processing time
  // 2: 1 + extracted features, used keypoints, localization variance, ego-motion and localization summary
  // 3: 2 + sub-problems processing duration
  // 4: 3 + ceres optimization summary
  // 5: 4 + logging/maps memory usage
  int Verbosity = 0;

  // Optional log of computed pose, localization covariance and keypoints of each
  // processed frame.
  // - A value of 0. will disable logging.
  // - A negative value will log all incoming data, without any timeout.
  // - A positive value will keep only the most recent data, forgetting all
  //   previous data older than LoggingTimeout seconds.
  // WARNING : A big value of LoggingTimeout may lead to an important memory
  //           consumption if SLAM is run for a long time.
  double LoggingTimeout = 0.;

  // Wether to use octree compression during keypoints logging.
  // This reduces about 5 times the memory consumption, but slows down logging (and PGO).
  PointCloudStorageType LoggingStorage = PointCloudStorageType::PCL_CLOUD;

  // Should the keypoints features maps be updated at each step.
  // It is usually set to true, but forbiding maps update can be usefull in case
  // of post-SLAM optimization with GPS and then run localization only in fixed
  // optimized map.
  bool UpdateMap = true;

  // Number of frames that have been processed
  unsigned int NbrFrameProcessed = 0;

  // Sequence id of the previous processed frame, used to check frames dropping
  unsigned int PreviousFrameSeq = 0;

  // ---------------------------------------------------------------------------
  //   Trajectory, transforms and undistortion
  // ---------------------------------------------------------------------------

  // **** COORDINATES SYSTEMS ****

  // Static transform to link BASE and LIDAR coordinates systems
  // It corresponds to the pose of LIDAR origin in BASE coordinates
  Eigen::Isometry3d BaseToLidarOffset = Eigen::Isometry3d::Identity();

  // Coordinates systems (CS) names to fill in pointclouds or poses headers
  std::string WorldFrameId = "world";  // CS of trajectory and maps
  std::string BaseFrameId;             // CS of current keypoints, defaults to input cloud frame_id if BaseToLidarOffset is unset, or BaseFrameIdDefault otherwise.
  const std::string BaseFrameIdDefault = "base";  // Default BASE name to use if BaseToLidarOffset is defined but not BaseFrameId.

  // **** LOCALIZATION ****

  // Global transformation to map the current pointcloud to the previous one
  Eigen::Isometry3d Trelative;

  // Transformation to map the current pointcloud in the world coordinates
  // This pose is the pose of BASE in WORLD coordinates, at the time
  // corresponding to the end of Lidar scan.
  Eigen::Isometry3d Tworld;
  Eigen::Isometry3d PreviousTworld;

  // [s] SLAM computation duration of last processed frame (~Tworld delay)
  // used to compute latency compensated pose
  double Latency;

  // **** UNDISTORTION ****

  // Pose at the beginning of current frame
  Eigen::Isometry3d TworldFrameStart;

  // Transform interpolator to estimate the pose of the sensor within a lidar
  // frame, using poses at the beginning and end of frame.
  LinearTransformInterpolator<double> WithinFrameMotion;

  // If Undistortion is enabled, it is necessary to save frame duration
  // (time ellapsed between first and last point measurements)
  double FrameDuration;

  // **** LOGGING ****

  // Computed trajectory of the sensor (the list of past computed poses,
  // covariances and keypoints of each frame).
  std::deque<Transform> LogTrajectory;
  std::deque<std::array<double, 36>> LogCovariances;
  std::deque<PointCloudStorage<Point>> LogEdgesPoints;
  std::deque<PointCloudStorage<Point>> LogPlanarsPoints;
  std::deque<PointCloudStorage<Point>> LogBlobsPoints;

  // ---------------------------------------------------------------------------
  //   Keypoints extraction and maps
  // ---------------------------------------------------------------------------

  // Current frame
  PointCloud::Ptr CurrentFrame;

  // Keypoints extractor
  std::shared_ptr<SpinningSensorKeypointExtractor> KeyPointsExtractor;

  // Raw extracted keypoints, in BASE coordinates (no undistortion)
  PointCloud::Ptr CurrentEdgesPoints;
  PointCloud::Ptr CurrentPlanarsPoints;
  PointCloud::Ptr CurrentBlobsPoints;
  PointCloud::Ptr PreviousEdgesPoints;
  PointCloud::Ptr PreviousPlanarsPoints;
  PointCloud::Ptr PreviousBlobsPoints;

  // Extracted keypoints, in WORLD coordinates (with undistortion if enabled)
  PointCloud::Ptr CurrentWorldEdgesPoints;
  PointCloud::Ptr CurrentWorldPlanarsPoints;
  PointCloud::Ptr CurrentWorldBlobsPoints;

  // keypoints local map
  std::shared_ptr<RollingGrid> EdgesPointsLocalMap;
  std::shared_ptr<RollingGrid> PlanarPointsLocalMap;
  std::shared_ptr<RollingGrid> BlobsPointsLocalMap;

  // ---------------------------------------------------------------------------
  //   Optimization data
  // ---------------------------------------------------------------------------

  //! Matching results
  std::map<Keypoint, KeypointsRegistration::MatchingResults> EgoMotionMatchingResults;
  std::map<Keypoint, KeypointsRegistration::MatchingResults> LocalizationMatchingResults;

  // Optimization results
  // Variance-Covariance matrix that estimates the localization error about the
  // 6-DoF parameters (DoF order : X, Y, Z, rX, rY, rZ)
  KeypointsRegistration::RegistrationError LocalizationUncertainty;

  // ---------------------------------------------------------------------------
  //   Optimization parameters
  // ---------------------------------------------------------------------------

  // The max distance allowed between a keypoint from the current frame and its
  // neighborhood from the map (or previous frame) to build an ICP match.
  // If the distance is over this limit, no match residual will be built.
  double MaxDistanceForICPMatching = 5.;

  // Maximum number of iteration
  // in the ego motion optimization step
  unsigned int EgoMotionLMMaxIter = 15;

  // Maximum number of iteration
  // in the localization optimization step
  unsigned int LocalizationLMMaxIter = 15;

  // During the Levenberg-Marquardt algoritm
  // keypoints will have to be match with planes
  // and lines of the previous frame. This parameter
  // indicates how many times we want to do the
  // the ICP matching
  unsigned int EgoMotionICPMaxIter = 4;
  unsigned int LocalizationICPMaxIter = 3;

  // When computing the point<->line and point<->plane distance
  // in the ICP, the kNearest edges/planes points of the current
  // points are selected to approximate the line/plane using a PCA
  // If the one of the k-nearest points is too far the neigborhood
  // is rejected. We also make a filter upon the ratio of the eigen
  // values of the variance-covariance matrix of the neighborhood
  // to check if the points are distributed upon a line or a plane
  unsigned int LocalizationLineDistanceNbrNeighbors = 10;
  unsigned int LocalizationMinimumLineNeighborRejection = 4;
  double LocalizationLineDistancefactor = 5.0;

  unsigned int LocalizationPlaneDistanceNbrNeighbors = 5;
  double LocalizationPlaneDistancefactor1 = 35.0;
  double LocalizationPlaneDistancefactor2 = 8.0;

  double LocalizationMaxPlaneDistance = 0.2;
  double LocalizationMaxLineDistance = 0.2;

  unsigned int LocalizationBlobDistanceNbrNeighbors = 25.;  // TODO : set from user interface

  unsigned int EgoMotionLineDistanceNbrNeighbors = 8;
  unsigned int EgoMotionMinimumLineNeighborRejection = 3;
  double EgoMotionLineDistancefactor = 5.;

  unsigned int EgoMotionPlaneDistanceNbrNeighbors = 5;
  double EgoMotionPlaneDistancefactor1 = 35.0;
  double EgoMotionPlaneDistancefactor2 = 8.0;

  double EgoMotionMaxPlaneDistance = 0.2;
  double EgoMotionMaxLineDistance = 0.2;

  double MinNbrMatchedKeypoints = 20.;  // TODO : set from user interface

  // Loss saturation properties
  // The loss function used is  L(residual) = scale * arctan(residual / scale)
  // where residual is the quality of each keypoints match.
  // TODO : simplify parameters setting
  double EgoMotionInitLossScale = 2.0 ;  // Saturation around 5 meters
  double EgoMotionFinalLossScale = 0.2 ; // Saturation around 1.5 meters
  double LocalizationInitLossScale = 0.7;     // Saturation around 2.5 meters
  double LocalizationFinalLossScale = 0.05;   // Saturation around 0.4 meters

  // ---------------------------------------------------------------------------
  //   Main sub-problems and methods
  // ---------------------------------------------------------------------------

  // Check that input frame is correct
  // (empty frame, same timestamp, frame dropping, ...)
  bool CheckFrame(const PointCloud::Ptr& inputPc);

  // Update current frame time field in prevision of undistortion
  void UpdateFrameTime(const PointCloud::Ptr& inputPc);

  // Extract keypoints from input pointcloud,
  // and transform them from LIDAR to BASE coordinate system.
  void ExtractKeypoints();

  // Estimate the ego motion since last frame.
  // Extrapolate new pose with a constant velocity model and/or
  // refine estimation by registering current frame keypoints on previous frame keypoints.
  void ComputeEgoMotion();

  // Compute the pose of the current frame in world referential by registering
  // current frame keypoints on keypoints from maps
  void Localization();

  // Update the maps by adding to the rolling grids the current keypoints
  // expressed in the world reference frame coordinate system
  void UpdateMapsUsingTworld();

  // Log current frame processing results : pose, covariance and keypoints.
  void LogCurrentFrameState(double time, const std::string& frameId);

  // ---------------------------------------------------------------------------
  //   Helpers
  // ---------------------------------------------------------------------------

  // All points of the current frame have been acquired at a different timestamp.
  // The goal is to express them in the same referential. This can be done using
  // estimated egomotion and assuming a constant angular velocity and velocity
  // during a sweep, or any other motion model.

  // Interpolate scan begin pose from PreviousTworld and Tworld.
  Eigen::Isometry3d InterpolateBeginScanPose();
};

} // end of LidarSlam namespace