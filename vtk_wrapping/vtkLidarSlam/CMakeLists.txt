
# Build dynamic vtk Module
set(BUILD_SHARED_LIBS ON)

set(lidarslam_classes
  vtkSlam
  vtkSlamManager
  vtkSpinningSensorKeypointExtractor
)

# Create Module
vtk_module_add_module(vtkLidarSlam
  CLASSES ${lidarslam_classes}
)

target_link_libraries(vtkLidarSlam PUBLIC LidarSlam)

target_include_directories(vtkLidarSlam
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)
