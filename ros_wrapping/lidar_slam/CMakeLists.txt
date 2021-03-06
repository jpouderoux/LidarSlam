cmake_minimum_required(VERSION 3.9)
project(lidar_slam VERSION 1.5)

# If you're not familiar with catkin, please have a look at these useful links.
# Catkin documentation:
#  https://docs.ros.org/en/api/catkin/html/howto/format2/index.html
# Catkin CMakeLists.txt ROS tutorial:
#  http://wiki.ros.org/catkin/CMakeLists.txt
# CMakeLists.txt template created by catkin_create_pkg:
#  https://github.com/ros-infrastructure/catkin_pkg/blob/master/src/catkin_pkg/templates/CMakeLists.txt.in

# Compile as C++14, supported in ROS Kinetic and newer
set(CMAKE_CXX_STANDARD 14)
if (NOT CMAKE_BUILD_TYPE)
    message(STATUS "No build type selected, default to RelWithDebInfo")
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Find catkin macros and libraries
find_package(catkin REQUIRED COMPONENTS
  roscpp
  tf2_ros
  pcl_ros
  pcl_conversions
  std_msgs
  geometry_msgs
  sensor_msgs
  nav_msgs
  message_generation
  apriltag_ros
)

################################################
## Declare ROS messages, services and actions ##
################################################

# Generate messages in the 'msg' folder
add_message_files(
  FILES
  SlamCommand.msg
  Confidence.msg
)

# Generate added messages and services with any dependencies listed here
generate_messages(
  DEPENDENCIES
  std_msgs
)

###################################
## catkin specific configuration ##
###################################

## The catkin_package macro generates cmake config files for your package
## Declare things to be passed to dependent projects
## INCLUDE_DIRS: uncomment this if your package contains header files
## LIBRARIES: libraries you create in this project that dependent projects also need
## CATKIN_DEPENDS: catkin_packages dependent projects also need
## DEPENDS: system dependencies of this project that dependent projects also need
catkin_package(
  LIBRARIES LidarSlam
  CATKIN_DEPENDS roscpp tf2_ros pcl_ros pcl_conversions std_msgs geometry_msgs sensor_msgs nav_msgs message_runtime apriltag_ros
)

###########
## Build ##
###########

# Build LidarSlam lib which lies in parent directory
add_subdirectory(../.. ${CMAKE_BINARY_DIR}/slam)

# Specify additional locations of header files
include_directories(${catkin_INCLUDE_DIRS})

# Add LiDAR SLAM ROS node
add_executable(lidar_slam_node
  src/LidarSlamNode.cxx
  src/LidarSlamNode_main.cxx
)
target_link_libraries(lidar_slam_node
  LidarSlam
  ${catkin_LIBRARIES}
)

#############
## Install ##
#############

install(TARGETS lidar_slam_node
        RUNTIME DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION})
install(DIRECTORY launch
        DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION})
install(DIRECTORY params
        DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION})
