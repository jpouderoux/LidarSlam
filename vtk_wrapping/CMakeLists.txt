find_package(VTK REQUIRED)

if(NOT LidarSlam_VERSION)
  message(FATAL_ERROR "LidarSlam_VERSION not defined")
endif()

# Create the VTK module
# First we scan the modules in our project to find out the dependency graph
# between them.
vtk_module_scan(
  # `vtk_module_find_modules` would be preferred.
  MODULE_FILES      "${CMAKE_CURRENT_SOURCE_DIR}/vtkLidarSlam/vtk.module"
  # Not building the only module we have is silly.
  REQUEST_MODULES   vtkLidarSlam
  # Store the list of provided modules from this scan.
  PROVIDES_MODULES  modules
  # Disable the tests for our modules.
  ENABLE_TESTS      OFF)

# Build the module we just scanned.
vtk_module_build(MODULES ${modules}
  LIBRARY_DESTINATION lib
  LICENSE_DESTINATION license)