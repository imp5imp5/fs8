cmake_minimum_required(VERSION 3.15)
include(ExternalProject)
project(fs8utils)
set(CMAKE_CONFIGURATION_TYPES "Release" "Debug")

if(MSVC)
  add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/MP>)
  add_compile_options(
    "$<$<CONFIG:RELEASE>:/MT>"
    "$<$<CONFIG:DEBUG>:/MTd>"
  )
endif()


file(GLOB SRC_PACK
  utils/fs8pack.cpp
  library/*.h
)

file(GLOB SRC_EXTRACT
  utils/fs8extract.cpp
  library/*.h
)


ExternalProject_Add( zstd
  GIT_REPOSITORY https://github.com/facebook/zstd.git
  GIT_TAG v1.5.0
  SOURCE_SUBDIR build/cmake
  CMAKE_ARGS
    -DCMAKE_INSTALL_PREFIX=bin
    -DCMAKE_INSTALL_LIBDIR=bin_lib
    -DZSTD_BUILD_PROGRAMS:BOOL=OFF
    -DZSTD_BUILD_SHARED:BOOL=OFF
    -DZSTD_MULTITHREAD_SUPPORT:BOOL=ON
    -DZSTD_USE_STATIC_RUNTIME:BOOL=ON
)

set(ZSTD_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/build/zstd-prefix/src/zstd-build/bin/include)
set(ZSTD_LIBRARY ${PROJECT_SOURCE_DIR}/build/zstd-prefix/src/zstd-build/bin/bin_lib)


include_directories(
  library
  ${ZSTD_INCLUDE_DIR}
)

add_executable(fs8pack ${SRC_PACK})
add_executable(fs8extract ${SRC_EXTRACT})
add_dependencies(fs8pack zstd)
add_dependencies(fs8extract zstd)

target_compile_features(fs8pack PRIVATE cxx_std_17)
target_compile_features(fs8extract PRIVATE cxx_std_17)

target_link_directories(fs8pack PUBLIC ${ZSTD_LIBRARY})
target_link_directories(fs8extract PUBLIC ${ZSTD_LIBRARY})


if(WIN32)
  target_link_libraries(fs8pack zstd_static)
  target_link_libraries(fs8extract zstd_static)
endif()

if(UNIX)
  target_link_libraries(fs8pack libzstd.a pthread)
  target_link_libraries(fs8extract libzstd.a pthread)
endif()
