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
# Dependency: ONNX Runtime (bundled)
# ==============================================================================
# ONNX Runtime is bundled with this plugin (windows/third_party/onnxruntime/).
# No download or manual setup required.
# To use a different version, set ONNXRUNTIME_ROOT CMake option.
# ==============================================================================
set(ONNXRUNTIME_VERSION "1.20.1" CACHE STRING "ONNX Runtime version")

set(_BUNDLED_ONNX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")

if(DEFINED ONNXRUNTIME_ROOT AND EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
  message(STATUS "[pp_ocr] Using user-provided ONNX Runtime: ${ONNXRUNTIME_ROOT}")
  set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/include")
  set(ONNXRUNTIME_LIB_DIR "${ONNXRUNTIME_ROOT}/lib")
elseif(EXISTS "${_BUNDLED_ONNX_DIR}/include/onnxruntime_cxx_api.h")
  message(STATUS "[pp_ocr] Using bundled ONNX Runtime v${ONNXRUNTIME_VERSION}")
  set(ONNXRUNTIME_INCLUDE_DIR "${_BUNDLED_ONNX_DIR}/include")
  set(ONNXRUNTIME_LIB_DIR "${_BUNDLED_ONNX_DIR}/lib")
else()
  message(FATAL_ERROR
    "[pp_ocr] ONNX Runtime not found.\n"
    "Bundled location missing: ${_BUNDLED_ONNX_DIR}\n"
    "Set -DONNXRUNTIME_ROOT=<path> to use a custom installation.")
endif()

if(NOT EXISTS "${ONNXRUNTIME_INCLUDE_DIR}/onnxruntime_cxx_api.h")
  message(FATAL_ERROR "[pp_ocr] ONNX Runtime headers not found at: ${ONNXRUNTIME_INCLUDE_DIR}")
endif()
message(STATUS "[pp_ocr] ONNX Runtime include: ${ONNXRUNTIME_INCLUDE_DIR}")
message(STATUS "[pp_ocr] ONNX Runtime lib: ${ONNXRUNTIME_LIB_DIR}")

# ==============================================================================
# Dependency: OpenCV (bundled)
# ==============================================================================
# OpenCV is bundled with this plugin (windows/third_party/opencv/).
# No download or manual setup required.
# To use a different version, set OpenCV_DIR CMake option.
# ==============================================================================
set(_BUNDLED_OPENCV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencv")

if(DEFINED OpenCV_DIR AND EXISTS "${OpenCV_DIR}/OpenCVConfig.cmake")
  message(STATUS "[pp_ocr] Using user-provided OpenCV: ${OpenCV_DIR}")
  find_package(OpenCV REQUIRED)
elseif(DEFINED ENV{OpenCV_DIR} AND EXISTS "$ENV{OpenCV_DIR}/OpenCVConfig.cmake")
  message(STATUS "[pp_ocr] Using OpenCV_DIR from env: $ENV{OpenCV_DIR}")
  set(OpenCV_DIR "$ENV{OpenCV_DIR}" CACHE PATH "" FORCE)
  find_package(OpenCV REQUIRED)
elseif(EXISTS "${_BUNDLED_OPENCV_DIR}/include/opencv2/opencv.hpp")
  message(STATUS "[pp_ocr] Using bundled OpenCV 4.9.0")
  set(OpenCV_INCLUDE_DIRS "${_BUNDLED_OPENCV_DIR}/include")
  # Use generator expression to select debug/release lib
  set(OpenCV_LIBS
    "$<$<CONFIG:Debug>:${_BUNDLED_OPENCV_DIR}/lib/opencv_world490d.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:${_BUNDLED_OPENCV_DIR}/lib/opencv_world490.lib>"
  )
  set(_OPENCV_DLL
    "$<$<CONFIG:Debug>:${_BUNDLED_OPENCV_DIR}/lib/opencv_world490d.dll>"
    "$<$<NOT:$<CONFIG:Debug>>:${_BUNDLED_OPENCV_DIR}/lib/opencv_world490.dll>"
  )

  # Auto-copy debug DLL from opencv-prebuilt if missing (too large for GitHub)
  set(_PREBUILT_DBG_DLL "${CMAKE_CURRENT_SOURCE_DIR}/../opencv-prebuilt/opencv/build/x64/vc16/bin/opencv_world490d.dll")
  set(_BUNDLED_DBG_DLL "${_BUNDLED_OPENCV_DIR}/lib/opencv_world490d.dll")
  if(NOT EXISTS "${_BUNDLED_DBG_DLL}" AND EXISTS "${_PREBUILT_DBG_DLL}")
    message(STATUS "[pp_ocr] Copying debug OpenCV DLL from opencv-prebuilt...")
    file(COPY "${_PREBUILT_DBG_DLL}" DESTINATION "${_BUNDLED_OPENCV_DIR}/lib")
  endif()
else()
  message(FATAL_ERROR
    "[pp_ocr] OpenCV not found.\n"
    "Bundled location missing: ${_BUNDLED_OPENCV_DIR}\n"
    "Set -DOpenCV_DIR=<path> to use a custom installation.")
endif()

message(STATUS "[pp_ocr] OpenCV include: ${OpenCV_INCLUDE_DIRS}")
message(STATUS "[pp_ocr] OpenCV libs: ${OpenCV_LIBS}")

# ==============================================================================
# Plugin library
# ==============================================================================

# Any new source files that you add to the plugin should be added here.
list(APPEND PLUGIN_SOURCES
  "paddle_ocr_plugin.cpp"
  "paddle_ocr_plugin.h"
  "ocr_engine.cpp"
  "ocr_engine.h"
  "pp_ocr_ffi.cpp"
  "pp_ocr_ffi.h"
)

# Define the plugin library target. Its name must not be changed.
add_library(${PLUGIN_NAME} SHARED
  "include/pp_ocr/paddle_ocr_plugin_c_api.h"
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
  "${OpenCV_INCLUDE_DIRS}"
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
set(_OPENCV_DLLS "")
# Include both debug and release DLLs; the correct one is selected at runtime.
foreach(_dll_name opencv_world490.dll opencv_world490d.dll)
  set(_dll_path "${_BUNDLED_OPENCV_DIR}/lib/${_dll_name}")
  if(EXISTS "${_dll_path}")
    list(APPEND _OPENCV_DLLS "${_dll_path}")
  endif()
endforeach()

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
