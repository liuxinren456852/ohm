find_package(Boost COMPONENTS "thread")
find_package(GLM)
find_package(LIBLAS QUIET)
find_package(LASZIP QUIET)
find_package(ZLIB)

set(OHM_UTILS_GPU_API_DEFAULT none)
if(OHM_BUILD_OPENCL)
  set(OHM_UTILS_GPU_API_DEFAULT OpenCL)
elseif(OHM_BUILD_CUDA)
  set(OHM_UTILS_GPU_API_DEFAULT CUDA)
else()
  message(FATAL_ERROR "No GPU API selected to build ohm utilities")
endif()

set(OHM_UTILS_GPU_API ${OHM_UTILS_GPU_API_DEFAULT} CACHE STRING "Select which GPU API the ohm utilities are built to use.")
set_property(CACHE OHM_UTILS_GPU_API PROPERTY STRINGS CUDA OpenCL)

if(OHM_UTILS_GPU_API_DEFAULT EQUAL "OpenCL")
  set(OHM_LIBRARY ohmocl)
elseif(OHM_UTILS_GPU_API_DEFAULT EQUAL "CUDA")
  set(OHM_LIBRARY ohmcuda)
endif()

add_subdirectory(ohm2ply)
add_subdirectory(ohmfilter)
add_subdirectory(ohmheightmap)
add_subdirectory(ohminfo)
add_subdirectory(ohmpop)
add_subdirectory(ohmprob)
add_subdirectory(ohmquery)
add_subdirectory(ohmsubmap)

if(OHM_BUILD_HEIGHTMAPUTIL)
  add_subdirectory(ohmhm2img)
endif(OHM_BUILD_HEIGHTMAPUTIL)

if(OHM_WITH_OCTOMAP)
  add_subdirectory(octopop)
endif(OHM_WITH_OCTOMAP)
