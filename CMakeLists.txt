cmake_minimum_required(VERSION 3.15)

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


find_package(Zstd REQUIRED HINTS ${FS8_ZSTD_PATH}/build/cmake)

find_path(
  ZSTD_INCLUDE_DIRS zstd.h HINTS ${FS8_ZSTD_PATH}/lib
)

# ZSTD_BUILD_STATIC
find_library(
  ZSTD_LIBRARIES zstd zstd_static HINTS ${FS8_ZSTD_PATH}/lib
)


file(GLOB SRC
  utils/fs8pack.cpp
  library/*.h
)

include_directories(
  library
  ${ZSTD_INCLUDE_DIRS}
)


if(WIN32)
  add_executable(fs8pack ${SRC} zstd_static)
endif()

if(UNIX)
  add_executable(fs8pack ${SRC} zstd_static)
  target_link_libraries(fs8pack pthread)
endif()
