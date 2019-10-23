//=========================================================================
//
// Copyright 2018 Kitware, Inc.
// Author: Guilbert Pierre (spguilbert@gmail.com)
// Date: 03-27-2018
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
//=========================================================================

// This slam algorithm is inspired by the LOAM algorithm:
// J. Zhang and S. Singh. LOAM: Lidar Odometry and Mapping in Real-time.
// Robotics: Science and Systems Conference (RSS). Berkeley, CA, July 2014.

// The algorithm is composed of three sequential steps:
//
// - Keypoints extraction: this step consists of extracting keypoints over
// the points clouds. To do that, the laser lines / scans are trated indepently.
// The laser lines are projected onto the XY plane and are rescale depending on
// their vertical angle. Then we compute their curvature and create two class of
// keypoints. The edges keypoints which correspond to points with a hight curvature
// and planar points which correspond to points with a low curvature.
//
// - Ego-Motion: this step consists of recovering the motion of the lidar
// sensor between two frames (two sweeps). The motion is modelized by a constant
// velocity and angular velocity between two frames (i.e null acceleration).
// Hence, we can parameterize the motion by a rotation and translation per sweep / frame
// and interpolate the transformation inside a frame using the timestamp of the points.
// Since the points clouds generated by a lidar are sparses we can't design a
// pairwise match between keypoints of two successive frames. Hence, we decided to use
// a closest-point matching between the keypoints of the current frame
// and the geometrics features derived from the keypoints of the previous frame.
// The geometrics features are lines or planes and are computed using the edges keypoints
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
// - Mapping: This step consists of refining the motion recovered in the Ego-Motion
// step and to add the new frame in the environment map. Thanks to the ego-motion
// recovered at the previous step it is now possible to estimate the new position of
// the sensor in the map. We use this estimation as an initial point (R0, T0) and we
// perform an optimization again using the keypoints of the current frame and the matched
// keypoints of the map (and not only the previous frame this time!). Once the position in the
// map has been refined from the first estimation it is then possible to update the map by
// adding the keypoints of the current frame into the map.
//
// In the following programs : "vtkSlam.h" and "vtkSlam.cxx" the lidar
// coordinate system {L} is a 3D coordinate system with its origin at the
// geometric center of the lidar. The world coordinate system {W} is a 3D
// coordinate system which coinciding with {L] at the initial position. The
// points will be denoted by the ending letter L or W if they belong to
// the corresponding coordinate system

// LOCAL
#include "vtkSlam.h"
#include "vtkSpinningSensorKeypointExtractor.h"

// STD
#include <algorithm>
#include <numeric>
#include <cstring>

// VTK
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkDataArray.h>
#include <vtkDoubleArray.h>
#include <vtkFloatArray.h>
#include <vtkInformation.h>
#include <vtkInformationVector.h>
#include <vtkMath.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkPolyLine.h>
#include <vtkSmartPointer.h>
#include <vtkStreamingDemandDrivenPipeline.h>
#include <vtkUnsignedCharArray.h>
#include <vtkUnsignedShortArray.h>
#include <vtkTransform.h>
#include <vtkPoints.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTable.h>

//-----------------------------------------------------------------------------
vtkStandardNewMacro(vtkSlam)

namespace {

//-----------------------------------------------------------------------------
template <typename T>
vtkSmartPointer<T> createArray(const std::string& Name, int NumberOfComponents = 1, int NumberOfTuples = 0)
{
  vtkSmartPointer<T> array = vtkSmartPointer<T>::New();
  array->SetNumberOfComponents(NumberOfComponents);
  array->SetNumberOfTuples(NumberOfTuples);
  array->SetName(Name.c_str());
  return array;
}

//-----------------------------------------------------------------------------
double Rad2Deg(double val)
{
  return val / vtkMath::Pi() * 180;
}

//-----------------------------------------------------------------------------
void PolyDataFromPointCloud(pcl::PointCloud<Slam::Point>::Ptr pc, vtkPolyData* poly)
{
  auto pts = vtkSmartPointer<vtkPoints>::New();
  poly->SetPoints(pts);
  for (size_t i = 0; i < pc->size(); i++)
  {
    Slam::Point p = pc->points[i];
    if (!poly->GetPoints())
      cout << "no point cloud" << endl;
    poly->GetPoints()->InsertNextPoint(p.x, p.y, p.z);
  }
  vtkNew<vtkIdTypeArray> cells;
  cells->SetNumberOfValues(pc->size() * 2);
  vtkIdType* ids = cells->GetPointer(0);
  for (vtkIdType i = 0; i < pc->size(); ++i)
  {
    ids[i * 2] = 1;
    ids[i * 2 + 1] = i;
  }

  auto cellArray = vtkSmartPointer<vtkCellArray>::New();
  cellArray->SetCells(pc->size(), cells.GetPointer());
  poly->SetVerts(cellArray);
}
}

//-----------------------------------------------------------------------------
void PointCloudFromPolyData(vtkPolyData* poly, pcl::PointCloud<Slam::Point>::Ptr pc)
{
  auto arrayPosition = poly->GetPoints() ;
  auto arrayTime = poly->GetPointData()->GetArray("adjustedtime");
  auto arrayLaserId = poly->GetPointData()->GetArray("laser_id");
  auto arrayIntensity = poly->GetPointData()->GetArray("intensity");
  for (vtkIdType i = 0; i < poly->GetNumberOfPoints(); i++)
  {
    double pos[3];
    poly->GetPoint(i, pos);
    Slam::Point p;
    p.x = pos[0];
    p.y = pos[1];
    p.z = pos[2];
    p.time = arrayTime->GetTuple1(i) * 1e-6; // time in second
    p.laserId = arrayLaserId->GetTuple1(i);
    p.intensity = arrayIntensity->GetTuple1(i);
    pc->push_back(p);
  }
}

//-----------------------------------------------------------------------------
template <typename T>
std::vector<size_t> sortIdx(const std::vector<T> &v)
{
  // initialize original index locations
  std::vector<size_t> idx(v.size());
  std::iota(idx.begin(), idx.end(), 0);

  // sort indexes based on comparing values in v
  std::sort(idx.begin(), idx.end(),
       [&v](size_t i1, size_t i2) {return v[i1] > v[i2];});

  return idx;
}

//-----------------------------------------------------------------------------
void vtkSlam::SetKeyPointsExtractor (vtkSpinningSensorKeypointExtractor* _arg)
{
  vtkSetObjectBodyMacro(KeyPointsExtractor,vtkSpinningSensorKeypointExtractor,_arg);
  this->SlamAlgo.SetKeyPointsExtractor(this->KeyPointsExtractor->GetExtractor());
}

//-----------------------------------------------------------------------------
std::vector<size_t> vtkSlam::GetLaserIdMapping(vtkTable *calib)
{
  auto array = vtkDataArray::SafeDownCast(calib->GetColumnByName("verticalCorrection"));
  std::vector<size_t> laserIdMapping;
  if (array)
  {
    std::vector<double> verticalCorrection;
    verticalCorrection.resize(array->GetNumberOfTuples());
    for (int i =0; i < array->GetNumberOfTuples(); ++i)
    {
      verticalCorrection[i] = array->GetTuple1(i);
    }
    laserIdMapping = sortIdx(verticalCorrection);
  }
  else
  {
    vtkErrorMacro(<< "The calibration data has no column named 'verticalCorrection'");
  }
  return laserIdMapping;
}

//-----------------------------------------------------------------------------
int vtkSlam::RequestData(vtkInformation *vtkNotUsed(request),
vtkInformationVector **inputVector, vtkInformationVector *outputVector)
{
  // Get the input
  vtkPolyData *input = vtkPolyData::GetData(inputVector[0]->GetInformationObject(0));
  auto* calib = vtkTable::GetData(inputVector[1]->GetInformationObject(0));
  std::vector<size_t> laserMapping = GetLaserIdMapping(calib);

  pcl::PointCloud<Slam::Point>::Ptr pc (new pcl::PointCloud<Slam::Point>);
  PointCloudFromPolyData(input, pc);

  this->SlamAlgo.AddFrame(pc, laserMapping);
  // output 0 - Current Frame
  vtkInformation *outInfo0 = outputVector->GetInformationObject(0);
  vtkPolyData *output0 = vtkPolyData::SafeDownCast(
      outInfo0->Get(vtkDataObject::DATA_OBJECT()));

  // get transform
  Transform Tworld = this->SlamAlgo.GetWorldTransform();
  vtkSmartPointer<vtkTransform> transform = vtkSmartPointer<vtkTransform>::New();
  transform->PostMultiply();
  transform->RotateX(Rad2Deg(Tworld.rx));
  transform->RotateY(Rad2Deg(Tworld.ry));
  transform->RotateZ(Rad2Deg(Tworld.rz));
  transform->Translate(Tworld.position);
  // create transform filter and transformt the current frame
  vtkSmartPointer<vtkTransformPolyDataFilter> transformFilter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
  transformFilter->SetInputData(input);
  transformFilter->SetTransform(transform);
  transformFilter->Update();
  output0->ShallowCopy(transformFilter->GetOutput());

  // add all debug information if displayMode == True
  if (this->DisplayMode == true)
  {
    std::unordered_map<std::string, std::vector<double> > debugArray =
        this->KeyPointsExtractor->GetExtractor()->GetDebugArray();
    for (const auto& it : debugArray)
    {
      auto array = createArray<vtkDoubleArray>(it.first.c_str(), 1, it.second.size());
      // memcpy is a better alternative than looping on all tuples
      std::memcpy(array->GetVoidPointer(0), it.second.data(), sizeof(double) * it.second.size());
      output0->GetPointData()->AddArray(array);
    }
  }

  // output 1 - Trajectory
//  Eigen::AngleAxisd m(RollPitchYawToMatrix(Tworld.rx, Tworld.ry, Tworld.rz));
//  this->Trajectory->PushBack(pc->points[0].time, m, Eigen::Vector3d(Tworld.position));
  auto *output1 = vtkPolyData::GetData(outputVector->GetInformationObject(1));
  output1->ShallowCopy(this->Trajectory);

  // add all debug information if displayMode == True
  if (this->DisplayMode == true)
  {
    std::unordered_map<std::string, double> debugInfo =
        this->SlamAlgo.GetDebugInformation();
    for (const auto& it : debugInfo)
    {
      auto array = this->Trajectory->GetPointData()->GetArray(it.first.c_str());
      array->InsertNextTuple1(it.second);
    }
  }

  auto array = this->Trajectory->GetPointData()->GetArray("Covariance");
  array->InsertNextTuple(this->SlamAlgo.GetTransformCovariance().data());

  // output 2 - Edges Points Map
  auto *EdgeMap = vtkPolyData::GetData(outputVector->GetInformationObject(2));
  PolyDataFromPointCloud(this->SlamAlgo.GetEdgesMap(), EdgeMap);

  // output 3 - Planar Points Map
  auto *PlanarMap = vtkPolyData::GetData(outputVector->GetInformationObject(3));
  PolyDataFromPointCloud(this->SlamAlgo.GetPlanarsMap(), PlanarMap);

  // output 4 - Blob Points Map
  auto *BlobMap = vtkPolyData::GetData(outputVector->GetInformationObject(4));
  PolyDataFromPointCloud(this->SlamAlgo.GetBlobsMap(), BlobMap);

  return 1;
}

//-----------------------------------------------------------------------------
void vtkSlam::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
  os << indent << "Slam Parameters: " << std::endl;
  vtkIndent paramIndent = indent.GetNextIndent();
  #define PrintParameter(param) os << paramIndent << #param << "\t" << this->SlamAlgo.Get##param() << std::endl;
  PrintParameter(EgoMotionLMMaxIter)
  PrintParameter(EgoMotionICPMaxIter)
  PrintParameter(MappingLMMaxIter)
  PrintParameter(MappingICPMaxIter)
  PrintParameter(EgoMotionLineDistanceNbrNeighbors)
  PrintParameter(EgoMotionLineDistancefactor)
  PrintParameter(MappingMaxLineDistance)
  PrintParameter(MappingPlaneDistanceNbrNeighbors)
  PrintParameter(MappingPlaneDistancefactor1)
  PrintParameter(MappingPlaneDistancefactor2)
  PrintParameter(MappingMaxPlaneDistance)
  PrintParameter(MaxDistanceForICPMatching)
  PrintParameter(EgoMotionMinimumLineNeighborRejection)
  PrintParameter(MappingMinimumLineNeighborRejection)
  PrintParameter(MappingLineMaxDistInlier)
  this->GetKeyPointsExtractor()->PrintSelf(os, indent);
}

//-----------------------------------------------------------------------------
vtkSlam::vtkSlam()
{
  this->SetNumberOfInputPorts(2);
  this->SetNumberOfOutputPorts(5);
  this->Reset();
}

//-----------------------------------------------------------------------------
void vtkSlam::Reset()
{
  this->SlamAlgo.Reset();

  // output of the vtk filter
  this->Trajectory = vtkSmartPointer<vtkPolyData>::New();

  this->Trajectory->GetPointData()->AddArray(createArray<vtkDoubleArray>("Covariance", 36));

  // add the required array in the trajectory
  if (this->DisplayMode)
  {
      this->Trajectory->GetPointData()->AddArray(createArray<vtkDoubleArray>("EgoMotion: edges used"));
      this->Trajectory->GetPointData()->AddArray(createArray<vtkDoubleArray>("EgoMotion: planes used"));
      this->Trajectory->GetPointData()->AddArray(createArray<vtkDoubleArray>("Mapping: edges used"));
      this->Trajectory->GetPointData()->AddArray(createArray<vtkDoubleArray>("Mapping: planes used"));
      this->Trajectory->GetPointData()->AddArray(createArray<vtkDoubleArray>("Mapping: blobs used"));
      this->Trajectory->GetPointData()->AddArray(createArray<vtkDoubleArray>("Mapping: variance error"));
  }
}

//-----------------------------------------------------------------------------
int vtkSlam::FillInputPortInformation(int port, vtkInformation *info)
{
  if ( port == 0 )
  {
    info->Set(vtkDataObject::DATA_TYPE_NAME(), "vtkPolyData" );
    return 1;
  }
  if ( port == 1 )
  {
    info->Set(vtkDataObject::DATA_TYPE_NAME(), "vtkTable" );
    return 1;
  }
  return 0;
}

//-----------------------------------------------------------------------------
void vtkSlam::SetVoxelGridLeafSizeEdges(double size)
{
  this->SlamAlgo.SetVoxelGridLeafSizeEdges(size);
  this->ParametersModificationTime.Modified();
}

//-----------------------------------------------------------------------------
void vtkSlam::SetVoxelGridLeafSizePlanes(double size)
{
  this->SlamAlgo.SetVoxelGridLeafSizePlanes(size);
  this->ParametersModificationTime.Modified();
}

//-----------------------------------------------------------------------------
void vtkSlam::SetVoxelGridLeafSizeBlobs(double size)
{
  this->SlamAlgo.SetVoxelGridLeafSizeBlobs(size);
  this->ParametersModificationTime.Modified();
}

//-----------------------------------------------------------------------------
void vtkSlam::SetVoxelGridSize(unsigned int size)
{
  this->SlamAlgo.SetVoxelGridSize(size);
  this->ParametersModificationTime.Modified();
}

//-----------------------------------------------------------------------------
void vtkSlam::SetVoxelGridResolution(double resolution)
{
  this->SlamAlgo.SetVoxelGridResolution(resolution);
  this->ParametersModificationTime.Modified();
}
