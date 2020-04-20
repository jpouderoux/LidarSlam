#ifndef LIDAR_SLAM_NODE_H
#define LIDAR_SLAM_NODE_H

// ROS
#include <ros/ros.h>
#include <velodyne_pointcloud/point_types.h>
#include <tf2_ros/transform_listener.h>
#include <tf2_ros/transform_broadcaster.h>
#include <tf2_ros/static_transform_broadcaster.h>
#include <pcl_ros/point_cloud.h>
#include <nav_msgs/Odometry.h>
#include <lidar_slam/SlamCommand.h>

// SLAM
#include <LidarSlam/Slam.h>

class LidarSlamNode
{
public:

  using PointV = velodyne_pointcloud::PointXYZIR;
  using CloudV = pcl::PointCloud<PointV>;  ///< Pointcloud published by velodyne driver
  using PointS = Slam::Point;    
  using CloudS = pcl::PointCloud<PointS>;  ///< Pointcloud needed by SLAM

  //----------------------------------------------------------------------------
  /*!
   * @brief     Constructor.
   * @param[in] nh      Public ROS node handle, used to init publisher/subscribers.
   * @param[in] priv_nh Private ROS node handle, used to access parameters.
   */
  LidarSlamNode(ros::NodeHandle& nh, ros::NodeHandle& priv_nh);

  //----------------------------------------------------------------------------
  /*!
   * @brief     New lidar frame callback, running SLAM and publishing TF.
   * @param[in] cloud New Lidar Frame, published by velodyne_pointcloud/cloud_node.
   */
  void ScanCallback(const CloudV& cloud);

  //----------------------------------------------------------------------------
  /*!
   * @brief     Optional GPS odom callback, accumulating poses for SLAM/GPS calibration.
   * @param[in] msg Converted GPS pose with its associated covariance.
   */
  void GpsCallback(const nav_msgs::Odometry& msg);

  //----------------------------------------------------------------------------
  /*!
   * @brief     Receive an external command to process, such as pose graph
   *            optimization, GPS/SLAM calibration, set SLAM pose etc.
   * @param[in] msg The command message.
   */
  void SlamCommandCallback(const lidar_slam::SlamCommand& msg);

private:

  //----------------------------------------------------------------------------
  /*!
   * @brief     Convert a Velodyne pointcloud to the slam expected pointcloud format.
   * @param[in] cloudV Velodyne pointcloud, published by velodyne_pointcloud/cloud_node.
   * @return    The converted slam pointcloud.
   *
   * Velodyne pointcloud has fields : x, y, z, intensity (float), ring (uint16).
   * Slam pointcloud has fields     : x, y, z, intensity (uint8), laserId (uint8), time (double).
   */
  CloudS::Ptr ConvertToSlamPointCloud(const CloudV& cloudV) const;

  //----------------------------------------------------------------------------
  /*!
   * @brief     Update transform offset between BASE and LIDAR using TF2
   * @param[in] lidarFrameId The input LiDAR pointcloud frame_id.
   * @param[in] pclStamp     The input pointcloud timestamp.
   */
  void UpdateBaseToLidarOffset(const std::string& lidarFrameId, uint64_t pclStamp);

  //----------------------------------------------------------------------------
  /*!
   * @brief     Publish TF and PoseWithCovariance.
   * @param[in] odomToBase  Transform from OdometryFrameId to TrackingFrameId to send.
   * @param[in] poseCovar   Covariance associated to full 6 DOF pose.
   *
   * NOTE : poseCovar encodes covariance for DoF in this order : (X, Y, Z, rX, rY, rZ)
   */
  void PublishTfOdom(const Transform& odomToBase, const std::array<double, 36>& poseCovar);

  //----------------------------------------------------------------------------
  /*!
   * @brief     Publish SLAM features maps.
   * @param[in] pclStamp Timestamp of the maps (number of µs since UNIX epoch).
   */
  void PublishFeaturesMaps(uint64_t pclStamp = 0) const;

  //----------------------------------------------------------------------------
  /*!
   * @brief     Get and fill Slam parameters from ROS parameters server.
   * @param[in] priv_nh Private ROS node handle to access parameters.
   */
  void SetSlamParameters(ros::NodeHandle& priv_nh);

  //----------------------------------------------------------------------------
  /*!
   * @brief Run GPS/SLAM calibration from recorded GPS and SLAM poses, and
   *        publish static TF to link OdometryFrameId to GPS frame.
   */
  void GpsSlamCalibration();

  //----------------------------------------------------------------------------
  /*!
   * @brief Run pose graph optimization from GPS and SLAM poses, correcting SLAM
   *        trajectory and maps, and publish optimized LiDAR trajectory and
   *        static TF to link OdometryFrameId to GPS frame.
   */
  void PoseGraphOptimization();

  //----------------------------------------------------------------------------

  // SLAM stuff
  Slam LidarSlam;
  std::vector<size_t> LaserIdMapping;
  double LidarFreq = 10.;
  unsigned int PreviousFrameId = 0;

  // Basic publishers & subscribers
  ros::Publisher PoseCovarPub;
  ros::Subscriber CloudSub;
  ros::Subscriber SlamCommandSub;

  // TF stuff
  std::string OdometryFrameId = "odom";  ///< Frame in which SLAM odometry and maps are expressed.
  std::string TrackingFrameId;           ///< Frame to track (default: input pointcloud frame_id; otherwise, ensure a valid TF tree is published).
  tf2_ros::Buffer TfBuffer;
  tf2_ros::TransformListener TfListener;
  tf2_ros::TransformBroadcaster TfBroadcaster;
  tf2_ros::StaticTransformBroadcaster StaticTfBroadcaster;

  // Optional saving of pointclouds to PCD files.
  PCDFormat PcdFormat = PCDFormat::BINARY_COMPRESSED;  ///< Save pointclouds as ascii/binary/binary_compressed PCD files.

  // Optional use of GPS data to calibrate output SLAM pose to world coordinates or to run pose graph optimization (PGO).
  bool UseGps = false;                          ///< Enable GPS data logging for Pose Graph Optimization or GPS/SLAM calibration.
  bool CalibrationNoRoll = false;               ///< DEBUG Impose GPS/SLAM calibration to have no roll angle.
  std::string PgoG2oFileName = "";              ///< Filename of g2o file where to save pose graph to optimize.
  std::deque<Transform> GpsPoses;               ///< Buffer of last received GPS poses.
  std::deque<std::array<double, 9>> GpsCovars;  ///< Buffer of last received GPS positions covariances.
  Eigen::Isometry3d BaseToGpsOffset = Eigen::Isometry3d::Identity();  ///< Pose of the GPS antenna in BASE coordinates.
  ros::Subscriber GpsOdomSub;
  bool SetSlamPoseFromGpsRequest = false;

  // Debug publishers
  ros::Publisher GpsPathPub, SlamPathPub;
  ros::Publisher OptimizedSlamTrajectoryPub;
  ros::Publisher SlamCloudPub;
  ros::Publisher EdgesPub, PlanarsPub, BlobsPub;
  bool PublishIcpTrajectories = false, PublishOptimizedTrajectory = false;
  bool PublishEdges = false, PublishPlanars = false, PublishBlobs = false;
};

#endif // LIDAR_SLAM_NODE_H