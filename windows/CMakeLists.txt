# The Flutter tooling requires that developers have a version of Visual Studio
# installed that includes CMake 3.14 or later. You should not increase this
# version, as doing so will cause the plugin to fail to compile for some
# customers of the plugin.
cmake_minimum_required(VERSION 3.14)

# Project-level configuration.
set(PROJECT_NAME "pp_ocr")
project(${PROJECT_NAME} LANGUAGES CXX)

# Explicitly opt in to modern CMake behaviors to avoid warnings with recent
# versions of CMake.
cmake_policy(VERSION 3.14...3.25)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "pp_ocr_plugin")

# ==============================================================================
# Dependency: ONNX Runtime
# ==============================================================================
# ONNX Runtime is bundled with this plugin (windows/third_party/onnxruntime/).
# No download or manual setup required.
# To use a different version, set ONNXRUNTIME_ROOT CMake option.
# ==============================================================================
set(ONNXRUNTIME_VERSION "1.20.1" CACHE STRING "ONNX Runtime version")

# Bundled ONNX Runtime location (ships with the plugin)
set(_BUNDLED_ONNX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")

if(DEFINED ONNXRUNTIME_ROOT AND EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
  message(STATUS "[paddle_ocr] Using user-provided ONNX Runtime: ${ONNXRUNTIME_ROOT}")
  set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/include")
  set(ONNXRUNTIME_LIB_DIR "${ONNXRUNTIME_ROOT}/lib")
elseif(EXISTS "${_BUNDLED_ONNX_DIR}/include/onnxruntime_cxx_api.h")
  message(STATUS "[paddle_ocr] Using bundled ONNX Runtime v${ONNXRUNTIME_VERSION}")
  set(ONNXRUNTIME_INCLUDE_DIR "${_BUNDLED_ONNX_DIR}/include")
  set(ONNXRUNTIME_LIB_DIR "${_BUNDLED_ONNX_DIR}/lib")
else()
  message(FATAL_ERROR
    "[paddle_ocr] ONNX Runtime not found.\n"
    "Bundled location missing: ${_BUNDLED_ONNX_DIR}\n"
    "Set -DONNXRUNTIME_ROOT=<path> to use a custom installation.")
endif()

if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
  message(FATAL_ERROR "[paddle_ocr] ONNX Runtime headers not found at: ${ONNXRUNTIME_INCLUDE_DIR}")
endif()
message(STATUS "[paddle_ocr] ONNX Runtime include: ${ONNXRUNTIME_INCLUDE_DIR}")
message(STATUS "[paddle_ocr] ONNX Runtime lib: ${ONNXRUNTIME_LIB_DIR}")

# ==============================================================================
# Dependency: OpenCV
# ==============================================================================
# Option 1: Set OpenCV_DIR to your OpenCV build directory
#   e.g. -DOpenCV_DIR="C:/opencv/build"
# Option 2: Set OpenCV_DIR environment variable
# Option 3: Build from source via FetchContent (slow first build, ~10-20 min)
# ==============================================================================

if(DEFINED OpenCV_DIR AND EXISTS "${OpenCV_DIR}/OpenCVConfig.cmake")
  message(STATUS "[paddle_ocr] Using OpenCV_DIR: ${OpenCV_DIR}")
  find_package(OpenCV REQUIRED)
elseif(DEFINED ENV{OpenCV_DIR} AND EXISTS "$ENV{OpenCV_DIR}/OpenCVConfig.cmake")
  message(STATUS "[paddle_ocr] Using OpenCV_DIR from env: $ENV{OpenCV_DIR}")
  set(OpenCV_DIR "$ENV{OpenCV_DIR}" CACHE PATH "" FORCE)
  find_package(OpenCV REQUIRED)
else()
  message(STATUS "[paddle_ocr] OpenCV_DIR not set, building OpenCV from source (first build will be slow)...")
  message(STATUS "[paddle_ocr] TIP: Set -DOpenCV_DIR=path/to/opencv/build to skip building from source.")
  include(FetchContent)
  FetchContent_Declare(
    opencv
    URL https://github.com/opencv/opencv/archive/refs/tags/4.10.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  set(BUILD_opencv_apps OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_python2 OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_python3 OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_PERF_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_java OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_js OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_dnn OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_ml OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_highgui OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_videoio OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_features2d OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_calib3d OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_flann OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_photo OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_stitching OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_video OFF CACHE BOOL "" FORCE)
  set(BUILD_opencv_objdetect OFF CACHE BOOL "" FORCE)
  set(WITH_CUDA OFF CACHE BOOL "" FORCE)
  set(WITH_FFMPEG OFF CACHE BOOL "" FORCE)
  set(WITH_GSTREAMER OFF CACHE BOOL "" FORCE)
  set(WITH_DIRECTX OFF CACHE BOOL "" FORCE)
  set(WITH_OPENCL OFF CACHE BOOL "" FORCE)
  set(WITH_ITT OFF CACHE BOOL "" FORCE)
  set(WITH_IPP OFF CACHE BOOL "" FORCE)
  set(WITH_LAPACK OFF CACHE BOOL "" FORCE)
  set(WITH_PROTOBUF OFF CACHE BOOL "" FORCE)
  set(WITH_EIGEN OFF CACHE BOOL "" FORCE)
  set(WITH_TBB OFF CACHE BOOL "" FORCE)
  set(WITH_OPENMP OFF CACHE BOOL "" FORCE)
  set(WITH_PTHREADS_PF OFF CACHE BOOL "" FORCE)
  set(OPENCV_SKIP_PYTHON WARNING CACHE BOOL "" FORCE)
  set(OPENCV_DISABLE_PYTHON_WARNING ON CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(OPENCV_ENABLE_NONFREE OFF CACHE BOOL "" FORCE)
  set(OPENCV_CONFIG_FILE_INCLUDE_DIR "${CMAKE_BINARY_DIR}/opencv_config" CACHE PATH "" FORCE)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/opencv_config")
  set(CMAKE_INSTALL_INCLUDEDIR "include")
  set(CMAKE_INSTALL_LIBDIR "lib")
  FetchContent_MakeAvailable(opencv)
endif()

message(STATUS "[paddle_ocr] OpenCV include: ${OpenCV_INCLUDE_DIRS}")
message(STATUS "[paddle_ocr] OpenCV libs: ${OpenCV_LIBS}")

# ==============================================================================
# Plugin library
# ==============================================================================

# Any new source files that you add to the plugin should be added here.
list(APPEND PLUGIN_SOURCES
  "paddle_ocr_plugin.cpp"
  "paddle_ocr_plugin.h"
  "ocr_engine.cpp"
  "ocr_engine.h"
)

# Define the plugin library target. Its name must not be changed.
add_library(${PLUGIN_NAME} SHARED
  "include/paddle_ocr/paddle_ocr_plugin_c_api.h"
  "paddle_ocr_plugin_c_api.cpp"
  ${PLUGIN_SOURCES}
)

# Apply a standard set of build settings.
apply_standard_settings(${PLUGIN_NAME})

# Symbols are hidden by default to reduce the chance of accidental conflicts.
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

# C++17 required for structured bindings and std::optional
target_compile_features(${PLUGIN_NAME} PRIVATE cxx_std_17)

# Source include directories and library dependencies.
target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${ONNXRUNTIME_INCLUDE_DIR}"
)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(${PLUGIN_NAME} PRIVATE
  flutter
  flutter_wrapper_plugin
  "${ONNXRUNTIME_LIB_DIR}/onnxruntime.lib"
  ${OpenCV_LIBS}
  comdlg32
)

# ==============================================================================
# Bundle DLLs with the plugin
# ==============================================================================
# Find OpenCV world DLL(s) for runtime bundling
# ==============================================================================
set(_OPENCV_DLLS "")
# OpenCV_LIB_PATH is set by pre-built OpenCV config (e.g. <build>/x64/vc16/lib)
if(OpenCV_LIB_PATH AND EXISTS "${OpenCV_LIB_PATH}")
  set(_OPENCV_BIN_DIR "${OpenCV_LIB_PATH}/../bin")
  get_filename_component(_OPENCV_BIN_DIR "${_OPENCV_BIN_DIR}" ABSOLUTE)
  file(GLOB _OPENCV_DLLS "${_OPENCV_BIN_DIR}/opencv_world*.dll")
  message(STATUS "[paddle_ocr] OpenCV DLLs found: ${_OPENCV_DLLS}")
else()
  message(STATUS "[paddle_ocr] OpenCV_LIB_PATH: ${OpenCV_LIB_PATH}")
  message(STATUS "[paddle_ocr] OpenCV_INSTALL_PATH: ${OpenCV_INSTALL_PATH}")
endif()

set(pp_ocr_bundled_libraries
  "${ONNXRUNTIME_LIB_DIR}/onnxruntime.dll"
  ${_OPENCV_DLLS}
  PARENT_SCOPE
)

# ==============================================================================
# Tests
# ==============================================================================
if (${include_${PROJECT_NAME}_tests})
set(TEST_RUNNER "${PROJECT_NAME}_test")
enable_testing()

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/release-1.11.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of googletest" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(${TEST_RUNNER}
  test/paddle_ocr_plugin_test.cpp
  ${PLUGIN_SOURCES}
)
apply_standard_settings(${TEST_RUNNER})
target_include_directories(${TEST_RUNNER} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${ONNXRUNTIME_INCLUDE_DIR}"
)
target_link_libraries(${TEST_RUNNER} PRIVATE flutter_wrapper_plugin)
target_link_libraries(${TEST_RUNNER} PRIVATE gtest_main gmock)
add_custom_command(TARGET ${TEST_RUNNER} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${FLUTTER_LIBRARY}" $<TARGET_FILE_DIR:${TEST_RUNNER}>
)

include(GoogleTest)
gtest_discover_tests(${TEST_RUNNER})
endif()
