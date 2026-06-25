# The Flutter tooling requires that developers have a version of Visual Studio
# installed that includes CMake 3.14 or later. You should not increase this
# version, as doing so will cause the plugin to fail to compile for some
# customers of the plugin.
cmake_minimum_required(VERSION 3.14)

# Project-level configuration.
set(PROJECT_NAME "paddle_ocr")
project(${PROJECT_NAME} LANGUAGES CXX)

# Explicitly opt in to modern CMake behaviors to avoid warnings with recent
# versions of CMake.
cmake_policy(VERSION 3.14...3.25)

# This value is used when generating builds using this plugin, so it must
# not be changed
set(PLUGIN_NAME "paddle_ocr_plugin")

# ==============================================================================
# Dependency: ONNX Runtime
# ==============================================================================
# Option 1: Set ONNXRUNTIME_ROOT to your local ONNX Runtime installation
#           e.g. -DONNXRUNTIME_ROOT="C:/onnxruntime-win-x64-1.20.1"
# Option 2: Auto-download (cached in build dir)
# ==============================================================================
set(ONNXRUNTIME_VERSION "1.20.1" CACHE STRING "ONNX Runtime version")

# Use the build directory for cache (works for pub.dev packages where
# plugin source is read-only)
set(_ONNX_CACHE_DIR "${CMAKE_BINARY_DIR}/onnxruntime-cache")

if(DEFINED ONNXRUNTIME_ROOT AND EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
  message(STATUS "[paddle_ocr] Using user-provided ONNX Runtime: ${ONNXRUNTIME_ROOT}")
else()
  set(ONNXRUNTIME_URL "https://github.com/microsoft/onnxruntime/releases/download/v${ONNXRUNTIME_VERSION}/onnxruntime-win-x64-${ONNXRUNTIME_VERSION}.zip")
  set(ONNXRUNTIME_ZIP "${_ONNX_CACHE_DIR}/onnxruntime-win-x64-${ONNXRUNTIME_VERSION}.zip")
  set(ONNXRUNTIME_EXTRACT_DIR "${_ONNX_CACHE_DIR}/onnxruntime-extracted")
  set(ONNXRUNTIME_ROOT "${ONNXRUNTIME_EXTRACT_DIR}/onnxruntime-win-x64-${ONNXRUNTIME_VERSION}")

  if(NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
    # Download with retry logic
    if(NOT EXISTS "${ONNXRUNTIME_ZIP}")
      message(STATUS "[paddle_ocr] Downloading ONNX Runtime v${ONNXRUNTIME_VERSION}...")
      message(STATUS "[paddle_ocr]   URL: ${ONNXRUNTIME_URL}")
      set(_DOWNLOAD_OK FALSE)
      foreach(_ATTEMPT RANGE 1 3)
        if(_DOWNLOAD_OK)
          break()
        endif()
        message(STATUS "[paddle_ocr]   Attempt ${_ATTEMPT}/3...")
        file(DOWNLOAD "${ONNXRUNTIME_URL}" "${ONNXRUNTIME_ZIP}"
             SHOW_PROGRESS
             TIMEOUT 300
             STATUS DOWNLOAD_STATUS
             TLS_VERIFY ON)
        list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
        if(STATUS_CODE EQUAL 0)
          set(_DOWNLOAD_OK TRUE)
          message(STATUS "[paddle_ocr]   Download complete.")
        else()
          list(GET DOWNLOAD_STATUS 1 STATUS_MSG)
          message(WARNING "[paddle_ocr]   Download failed: ${STATUS_MSG}")
          file(REMOVE "${ONNXRUNTIME_ZIP}")
        endif()
      endforeach()

      if(NOT _DOWNLOAD_OK)
        message(FATAL_ERROR
          "[paddle_ocr] Failed to download ONNX Runtime after 3 attempts.\n"
          "This is likely a network issue (common in China).\n"
          "\n"
          "Solutions:\n"
          "  1. Manually download from:\n"
          "     ${ONNXRUNTIME_URL}\n"
          "  2. Extract and set CMake option:\n"
          "     -DONNXRUNTIME_ROOT=\"<path-to-extracted-onnxruntime>\"\n"
          "  3. Or set environment variable before build:\n"
          "     set ONNXRUNTIME_ROOT=<path-to-extracted-onnxruntime>\n"
          "\n"
          "The extracted directory must contain 'include/' and 'lib/' subdirs.")
      endif()
    else()
      message(STATUS "[paddle_ocr] Using cached ONNX Runtime zip.")
    endif()

    # Verify zip is valid before extracting
    message(STATUS "[paddle_ocr] Extracting ONNX Runtime...")
    file(ARCHIVE_EXTRACT INPUT "${ONNXRUNTIME_ZIP}"
         DESTINATION "${ONNXRUNTIME_EXTRACT_DIR}")
  else()
    message(STATUS "[paddle_ocr] ONNX Runtime already extracted at: ${ONNXRUNTIME_ROOT}")
  endif()
endif()

set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/include")
set(ONNXRUNTIME_LIB_DIR "${ONNXRUNTIME_ROOT}/lib")

if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
  message(FATAL_ERROR
    "[paddle_ocr] ONNX Runtime headers not found at: ${ONNXRUNTIME_INCLUDE_DIR}\n"
    "\n"
    "The download or extraction may have failed.\n"
    "Try one of:\n"
    "  1. Delete ${_ONNX_CACHE_DIR} and rebuild\n"
    "  2. Manually download and set -DONNXRUNTIME_ROOT=<path>")
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
    URL https://github.com/opencv/opencv/archive/refs/tags/4.9.0.zip
    URL_HASH MD5=872cf2ded2c5e79cb5904563c0b35bf4
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
  set(WITH_CUDA OFF CACHE BOOL "" FORCE)
  set(WITH_FFMPEG OFF CACHE BOOL "" FORCE)
  set(WITH_GSTREAMER OFF CACHE BOOL "" FORCE)
  set(WITH_DIRECTX OFF CACHE BOOL "" FORCE)
  set(WITH_OPENCL OFF CACHE BOOL "" FORCE)
  set(WITH_ITT OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(OPENCV_ENABLE_NONFREE OFF CACHE BOOL "" FORCE)
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

set(paddle_ocr_bundled_libraries
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
