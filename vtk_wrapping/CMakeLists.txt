find_package(VTK REQUIRED)

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
  INSTALL_HEADERS ON
  LIBRARY_DESTINATION lib
  LICENSE_DESTINATION license)

# Wrap it with Python.
vtk_module_python_default_destination(python_destination)

vtk_module_wrap_python(
  MODULES         ${modules}
  PYTHON_PACKAGE  "LidarSlam"
  MODULE_DESTINATION "lib/${python_destination}"
  # Static Python modules are almost never wanted.
  BUILD_STATIC    OFF
  INSTALL_HEADERS OFF)

# Create an `__init__.py` for the wrapped module.
file(WRITE "${CMAKE_BINARY_DIR}/lib/${python_destination}/LidarSlam/__init__.py" "")

install(FILES "${CMAKE_BINARY_DIR}/lib/${python_destination}/LidarSlam/__init__.py"
        DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/${python_destination}/LidarSlam/")
