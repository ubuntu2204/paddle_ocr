# Flutter 工具链要求开发者安装包含 CMake 3.14 或更高版本的 Visual Studio。
# 不应提高此版本要求，否则会导致部分插件用户无法编译此插件。
cmake_minimum_required(VERSION 3.14)

# 项目级配置
set(PROJECT_NAME "pp_ocr")
project(${PROJECT_NAME} LANGUAGES CXX)

# 显式启用现代 CMake 行为，避免在较新版本 CMake 上产生警告
cmake_policy(VERSION 3.14...3.25)

# 此值在使用此插件生成构建时使用，不得更改
set(PLUGIN_NAME "pp_ocr_plugin")

# ==============================================================================
# 依赖项：ONNX Runtime（内嵌）
# ==============================================================================
# ONNX Runtime 随本插件内嵌分发（windows/third_party/onnxruntime/）。
# 无需下载或手动配置。
# 如需使用其他版本，设置 ONNXRUNTIME_ROOT CMake 选项即可。
# ==============================================================================
set(ONNXRUNTIME_VERSION "1.20.1" CACHE STRING "ONNX Runtime version")

set(_BUNDLED_ONNX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")

# 优先使用用户提供的 ONNX Runtime 路径，否则使用内嵌版本
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
# 依赖项：OpenCV（内嵌）
# ==============================================================================
# OpenCV 随本插件内嵌分发（windows/third_party/opencv/）。
# 无需下载或手动配置。
# 如需使用其他版本，设置 OpenCV_DIR CMake 选项即可。
# ==============================================================================
set(_BUNDLED_OPENCV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencv")

# 优先使用用户提供的 OpenCV 路径（CMake 变量或环境变量），否则使用内嵌版本
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
  # 使用生成器表达式按 Debug/Release 配置选择对应库
  set(OpenCV_LIBS
    "$<$<CONFIG:Debug>:${_BUNDLED_OPENCV_DIR}/lib/opencv_world490d.lib>"
    "$<$<NOT:$<CONFIG:Debug>>:${_BUNDLED_OPENCV_DIR}/lib/opencv_world490.lib>"
  )
  set(_OPENCV_DLL
    "$<$<CONFIG:Debug>:${_BUNDLED_OPENCV_DIR}/lib/opencv_world490d.dll>"
    "$<$<NOT:$<CONFIG:Debug>>:${_BUNDLED_OPENCV_DIR}/lib/opencv_world490.dll>"
  )

  # 缺失时自动从 opencv-prebuilt 复制 Debug DLL（文件过大，未纳入 GitHub 仓库）
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
# 插件库
# ==============================================================================

# 添加到插件的新源文件应在此处添加
list(APPEND PLUGIN_SOURCES
  "paddle_ocr_plugin.cpp"
  "paddle_ocr_plugin.h"
  "ocr_engine.cpp"
  "ocr_engine.h"
  "pp_ocr_ffi.cpp"
  "pp_ocr_ffi.h"
)

# 定义插件库目标，其名称不得更改
add_library(${PLUGIN_NAME} SHARED
  "include/pp_ocr/paddle_ocr_plugin_c_api.h"
  "paddle_ocr_plugin_c_api.cpp"
  ${PLUGIN_SOURCES}
)

# 应用标准构建设置
apply_standard_settings(${PLUGIN_NAME})

# 默认隐藏符号，降低意外冲突的概率
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

# 结构化绑定和 std::optional 需要 C++17
target_compile_features(${PLUGIN_NAME} PRIVATE cxx_std_17)

# 源文件包含目录和库依赖
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
# 将 DLL 与插件打包
# ==============================================================================
set(_OPENCV_DLLS "")
# 同时包含 Debug 和 Release 的 DLL；运行时将选择正确的版本
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
# 测试
# ==============================================================================
if (${include_${PROJECT_NAME}_tests})
set(TEST_RUNNER "${PROJECT_NAME}_test")
enable_testing()

# 通过 FetchContent 下载 GoogleTest 1.11.0
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/release-1.11.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "Disable installation of googletest" FORCE)
FetchContent_MakeAvailable(googletest)

# 构建测试可执行文件
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
# 构建后将 Flutter 库复制到测试输出目录
add_custom_command(TARGET ${TEST_RUNNER} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${FLUTTER_LIBRARY}" $<TARGET_FILE_DIR:${TEST_RUNNER}>
)

include(GoogleTest)
gtest_discover_tests(${TEST_RUNNER})
endif()
