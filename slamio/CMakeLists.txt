# Note: This library is a means to an end and is not well maintained.

include(GenerateExportHeader)

if(CLOUD_IO_LIB STREQUAL "none")
  message("No point cloud library available. Skipping slamio")
  return()
endif(CLOUD_IO_LIB STREQUAL "none")

find_package(GLM)

set(SOURCES
  SlamCloudLoader.h
)

set(PUBLIC_HEADERS
  SlamCloudLoader.h
  "${CMAKE_CURRENT_BINARY_DIR}/slamio/SlamIOExport.h"
)

# Need to add boost link directories because of LIBLAS.
if(CLOUD_IO_LIB STREQUAL "liblas")
  find_package(Boost COMPONENTS "thread")
  list(APPEND SOURCES
    SlamCloudLoader_las.cpp
  )
endif(CLOUD_IO_LIB STREQUAL "liblas")

if(CLOUD_IO_LIB STREQUAL "pdal")
  # Need to check PDAL version. We have can use a streaming loader with PDAL 1.7+
  message(STATUS "PDAL_VERSION: ${PDAL_VERSION}")
  if(PDAL_VERSION VERSION_GREATER_EQUAL 1.7)
    # 
    list(APPEND SOURCES
      SlamCloudLoader_pdal_stream.cpp
    )
  else(PDAL_VERSION VERSION_GREATER_EQUAL 1.7)
    list(APPEND SOURCES
      SlamCloudLoader_pdal.cpp
    )
  endif(PDAL_VERSION VERSION_GREATER_EQUAL 1.7)
endif(CLOUD_IO_LIB STREQUAL "pdal")

add_library(slamio ${SOURCES})
# More things pointing to the need for a refactor here
if(MSVC)
  target_compile_definitions(slamio PUBLIC NOMINMAX _USE_MATH_DEFINES)
endif(MSVC)

clang_tidy_target(slamio)

generate_export_header(slamio
      EXPORT_MACRO_NAME slamio_API
      EXPORT_FILE_NAME slamio/SlamIOExport.h
      STATIC_DEFINE slamio_STATIC)

target_include_directories(slamio
  PUBLIC
    $<INSTALL_INTERFACE:${OHM_PREFIX_INCLUDE}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/slamio>
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
)

target_include_directories(slamio SYSTEM
  PUBLIC "${GLM_INCLUDE_DIR}"
)

target_link_libraries(slamio PUBLIC ohmutil)

if(CLOUD_IO_LIB STREQUAL "pdal")
  target_link_libraries(slamio PUBLIC ${PDAL_LIBRARIES})
endif(CLOUD_IO_LIB STREQUAL "pdal")

if(CLOUD_IO_LIB STREQUAL "liblas")
  target_include_directories(slamio SYSTEM
    PRIVATE "${LIBLAS_INCLUDE_DIR}"
  )
  # Not the best CMake use, but it avoids some Boost link issues.
  link_directories("${Boost_LIBRARY_DIRS}")
  target_link_libraries(slamio PUBLIC
    ${LIBLAS_LIBRARIES}
    ${Boost_LIBRARIES}
  )

  if(LASZIP_FOUND)
    target_include_directories(slamio SYSTEM PRIVATE "${LASZIP_INCLUDE_DIR}")
    target_link_libraries(slamio PUBLIC ${LASZIP_LIBRARIES})
  endif(LASZIP_FOUND)
endif(CLOUD_IO_LIB STREQUAL "liblas")

install(TARGETS slamio EXPORT ${CMAKE_PROJECT_NAME}-config-targets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
  INCLUDES DESTINATION ${OHM_PREFIX_INCLUDE}/slamio
)

install(FILES ${PUBLIC_HEADERS} DESTINATION ${OHM_PREFIX_INCLUDE}/slamio)

source_group("source" REGULAR_EXPRESSION ".*$")
# Needs CMake 3.8+:
# source_group(TREE "${CMAKE_CURRENT_LIST_DIR}" PREFIX source FILES ${SOURCES})
