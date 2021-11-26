cmake_minimum_required(VERSION 3.15)
include(ExternalProject)

project(fs8pack)
set(CMAKE_CONFIGURATION_TYPES "Release" "Debug")

if(MSVC)
  add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/MP>)
  if(DASBOX_USE_STATIC_STD_LIBS)
    add_compile_options(
      "$<$<CONFIG:RELEASE>:/MT>"
      "$<$<CONFIG:DEBUG>:/MTd>"
    )
  endif()
endif()


file(GLOB SRC
  utils/fs8pack.cpp
  library/*.h
)


ExternalProject_Add( zstd
	GIT_REPOSITORY https://github.com/facebook/zstd.git
	GIT_TAG v1.5.0
	SOURCE_SUBDIR build/cmake
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=bin -DCMAKE_INSTALL_LIBDIR=bin_lib
)

set(ZSTD_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/build/zstd-prefix/src/zstd-build/bin/include)
set(ZSTD_LIBRARY ${PROJECT_SOURCE_DIR}/build/zstd-prefix/src/zstd-build/bin/bin_lib)


include_directories(
  library
  ${ZSTD_INCLUDE_DIR}
)

add_executable(fs8pack ${SRC})
add_dependencies(fs8pack zstd)

target_link_directories(fs8pack PUBLIC ${ZSTD_LIBRARY})


if(WIN32)
  target_link_libraries(fs8pack zstd_static)
endif()

if(UNIX)
  target_link_libraries(fs8pack zstd_static pthread)
endif()
