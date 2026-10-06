# The Flutter tooling requires that developers have CMake 3.13 or later
# installed on Linux. Do not increase this version.
cmake_minimum_required(VERSION 3.13)

# Project-level configuration.
set(PROJECT_NAME "pp_ocr")
project(${PROJECT_NAME} LANGUAGES CXX)

# This value is used when generating builds using this plugin, so it must
# not be changed.
set(PLUGIN_NAME "pp_ocr_plugin")

# ==============================================================================
# 依赖项：ONNX Runtime
# ==============================================================================
# Linux 上默认使用系统安装的 ONNX Runtime：
#   * 头文件：/usr/include/onnxruntime 或 /usr/local/include/onnxruntime
#   * 库文件：libonnxruntime.so
# 用户也可通过 -DONNXRUNTIME_ROOT=<path> 指向自定义安装位置，或将
# 预编译产物放置在 linux/third_party/onnxruntime/ 下（结构与 Windows
# 端保持一致：include/ 与 lib/）。
# ==============================================================================
set(ONNXRUNTIME_VERSION "1.20.1" CACHE STRING "ONNX Runtime version")
set(_BUNDLED_ONNX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/onnxruntime")

if(DEFINED ONNXRUNTIME_ROOT AND EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
  message(STATUS "[pp_ocr] Using user-provided ONNX Runtime: ${ONNXRUNTIME_ROOT}")
  set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_ROOT}/include")
  set(ONNXRUNTIME_LIB_DIR "${ONNXRUNTIME_ROOT}/lib")
  set(ONNXRUNTIME_LINK_TARGET "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.so")
elseif(EXISTS "${_BUNDLED_ONNX_DIR}/include/onnxruntime_cxx_api.h")
  message(STATUS "[pp_ocr] Using bundled ONNX Runtime v${ONNXRUNTIME_VERSION} (linux/third_party)")
  set(ONNXRUNTIME_INCLUDE_DIR "${_BUNDLED_ONNX_DIR}/include")
  set(ONNXRUNTIME_LIB_DIR "${_BUNDLED_ONNX_DIR}/lib")
  set(ONNXRUNTIME_LINK_TARGET "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.so")
else()
  # 回退到系统安装：先尝试 pkg-config，再退化到常见安装路径。
  find_package(PkgConfig QUIET)
  set(ONNXRUNTIME_LINK_TARGET "")
  if(PkgConfig_FOUND)
    pkg_check_modules(ORT QUIET onnxruntime)
  endif()
  if(ORT_FOUND)
    message(STATUS "[pp_ocr] Using system ONNX Runtime via pkg-config: ${ORT_VERSION}")
    set(ONNXRUNTIME_INCLUDE_DIR ${ORT_INCLUDE_DIRS})
    set(ONNXRUNTIME_LINK_TARGET ${ORT_LDFLAGS})
  else()
    # 常见 Debian/Ubuntu 安装：/usr/include/onnxruntime 与 /usr/lib/x86_64-linux-gnu/libonnxruntime.so
    find_path(ONNXRUNTIME_SYSTEM_INCLUDE
      NAMES onnxruntime_cxx_api.h
      PATHS /usr/include/onnxruntime /usr/local/include/onnxruntime
            /usr/include /usr/local/include)
    find_library(ONNXRUNTIME_SYSTEM_LIB
      NAMES onnxruntime
      PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)
    if(NOT ONNXRUNTIME_SYSTEM_INCLUDE OR NOT ONNXRUNTIME_SYSTEM_LIB)
      message(FATAL_ERROR
        "[pp_ocr] ONNX Runtime not found on Linux.\n"
        "Options:\n"
        "  1) Install system package (apt install libonnxruntime-dev), or\n"
        "  2) Extract Microsoft's prebuilt tarball to linux/third_party/onnxruntime/{include,lib}, or\n"
        "  3) Pass -DONNXRUNTIME_ROOT=<path-with-include-and-lib>.")
    endif()
    message(STATUS "[pp_ocr] Using system ONNX Runtime: ${ONNXRUNTIME_SYSTEM_LIB}")
    set(ONNXRUNTIME_INCLUDE_DIR "${ONNXRUNTIME_SYSTEM_INCLUDE}")
    set(ONNXRUNTIME_LINK_TARGET "${ONNXRUNTIME_SYSTEM_LIB}")
  endif()
endif()

message(STATUS "[pp_ocr] ONNX Runtime include: ${ONNXRUNTIME_INCLUDE_DIR}")
message(STATUS "[pp_ocr] ONNX Runtime link target: ${ONNXRUNTIME_LINK_TARGET}")

# ==============================================================================
# 依赖项：OpenCV
# ==============================================================================
# Linux 上默认使用系统安装的 OpenCV（apt install libopencv-dev）。
# 也可通过 -DOpenCV_DIR=<path> 指向自定义位置。
# 若插件目录下存在 linux/third_party/opencv/，则优先使用。
# ==============================================================================
set(_BUNDLED_OPENCV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/opencv")

if(EXISTS "${_BUNDLED_OPENCV_DIR}/lib/libopencv_core.so"
   OR EXISTS "${_BUNDLED_OPENCV_DIR}/include/opencv2/opencv.hpp")
  message(STATUS "[pp_ocr] Using bundled OpenCV at linux/third_party/opencv")
  set(OpenCV_INCLUDE_DIRS "${_BUNDLED_OPENCV_DIR}/include")
  file(GLOB _BUNDLED_OPENCV_LIBS "${_BUNDLED_OPENCV_DIR}/lib/libopencv_*.so")
  set(OpenCV_LIBS ${_BUNDLED_OPENCV_LIBS})
else()
  message(STATUS "[pp_ocr] Searching for system OpenCV (>= 4.0)...")
  find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
  message(STATUS "[pp_ocr] Found OpenCV ${OpenCV_VERSION} at ${OpenCV_DIR}")
endif()

# ==============================================================================
# GTK（供文件选择对话框使用；Flutter Linux 应用宿主本身依赖 GTK）
# ==============================================================================
find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk+-3.0)

# ==============================================================================
# 插件源文件（Linux 端 + 共享的 cpp/ 源码）
# ==============================================================================
set(PLUGIN_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cpp")

list(APPEND PLUGIN_SOURCES
  "paddle_ocr_plugin.cc"
  "include/pp_ocr/paddle_ocr_plugin.h"
  "${PLUGIN_SHARED_DIR}/ocr_engine.cpp"
  "${PLUGIN_SHARED_DIR}/ocr_engine.h"
  "${PLUGIN_SHARED_DIR}/pp_ocr_ffi.cpp"
  "${PLUGIN_SHARED_DIR}/pp_ocr_ffi.h"
  "${PLUGIN_SHARED_DIR}/debug_utils.h"
)

add_library(${PLUGIN_NAME} SHARED
  ${PLUGIN_SOURCES}
)

# Flutter 提供的标准构建设置（Linux 上默认 C++14，此处覆盖为 C++17）
apply_standard_settings(${PLUGIN_NAME})
target_compile_features(${PLUGIN_NAME} PRIVATE cxx_std_17)

# 隐藏符号，只显式导出必要 API（FLUTTER_PLUGIN_EXPORT / PP_OCR_EXPORT）
set_target_properties(${PLUGIN_NAME} PROPERTIES
  CXX_VISIBILITY_PRESET hidden)
target_compile_definitions(${PLUGIN_NAME} PRIVATE FLUTTER_PLUGIN_IMPL)

target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${PLUGIN_SHARED_DIR}"
  "${ONNXRUNTIME_INCLUDE_DIR}"
  "${OpenCV_INCLUDE_DIRS}"
)
target_include_directories(${PLUGIN_NAME} INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(${PLUGIN_NAME} PRIVATE
  flutter
  PkgConfig::GTK
  ${ONNXRUNTIME_LINK_TARGET}
  ${OpenCV_LIBS}
)

# 向下游（example/runner）传递 linker 搜索路径：因为 libpp_ocr_plugin.so 的
# DT_NEEDED 中会包含 libonnxruntime.so.1（来自 SONAME），链接 runner 时 ld
# 需要能找到该文件，否则报 "needed by ..., not found"。使用 -rpath-link 仅
# 参导搜索，不会污染产物 RPATH。
get_filename_component(_ort_link_dir "${ONNXRUNTIME_LINK_TARGET}" DIRECTORY)
if(_ort_link_dir AND NOT _ort_link_dir STREQUAL "")
  target_link_options(${PLUGIN_NAME} INTERFACE "LINKER:-rpath-link,${_ort_link_dir}")
endif()

# 同理处理 OpenCV 库目录（通常位于 /usr/lib/x86_64-linux-gnu，系统链接器
# 默认会搜索；但 bundled 方式时需要提醒链接器）。
if(OpenCV_LIBS)
  list(GET OpenCV_LIBS 0 _opencv_first_lib)
  if(_opencv_first_lib MATCHES "\.so")
    get_filename_component(_opencv_link_dir "${_opencv_first_lib}" DIRECTORY)
    if(EXISTS "${_opencv_link_dir}")
      target_link_options(${PLUGIN_NAME} INTERFACE "LINKER:-rpath-link,${_opencv_link_dir}")
    endif()
  endif()
endif()

# 让生成的 .so 在运行时通过 $ORIGIN 查找同目录下的依赖（若随插件打包）
set_target_properties(${PLUGIN_NAME} PROPERTIES
  INSTALL_RPATH "$ORIGIN;$ORIGIN/lib"
  BUILD_WITH_INSTALL_RPATH TRUE)

# ==============================================================================
# 随插件打包的动态库
# ==============================================================================
# 若使用系统安装（apt），留空即可；Flutter 工具链会自动通过 $ORIGIN 定位。
# 若使用 bundled 或 ONNXRUNTIME_ROOT，则把对应的 .so 拷贝到应用产物目录，
# 目标运行时同样通过 $ORIGIN 定位。
set(pp_ocr_bundled_libraries "")

# 内置（linux/third_party）或自定义 ONNXRUNTIME_ROOT 时都需拷贝 .so；
# glob 匹配 libonnxruntime.so* 以同时包含带版本号的主库、无后缀的
# 开发链接名以及 provider 插件。
if(DEFINED ONNXRUNTIME_LIB_DIR AND EXISTS "${ONNXRUNTIME_LIB_DIR}")
  file(GLOB _ort_bundled "${ONNXRUNTIME_LIB_DIR}/libonnxruntime*.so*")
  list(APPEND pp_ocr_bundled_libraries ${_ort_bundled})
endif()

if(DEFINED _BUNDLED_OPENCV_DIR AND EXISTS "${_BUNDLED_OPENCV_DIR}/lib")
  file(GLOB _bundled_opencv_so "${_BUNDLED_OPENCV_DIR}/lib/libopencv_*.so*")
  list(APPEND pp_ocr_bundled_libraries ${_bundled_opencv_so})
endif()

list(REMOVE_DUPLICATES pp_ocr_bundled_libraries)
set(pp_ocr_bundled_libraries ${pp_ocr_bundled_libraries} PARENT_SCOPE)

# ==============================================================================
# 测试（可选；当前未提供 Linux 单元测试，保留目录结构同与 Windows 对齐）
# ==============================================================================
# 先安全地把开关展开到一个局部变量，避免 include_${PROJECT_NAME}_tests 未定义
# 时 if() 因空参数而语法错误。
if(DEFINED include_${PROJECT_NAME}_tests)
  set(_pp_ocr_tests_enabled ${include_${PROJECT_NAME}_tests})
else()
  set(_pp_ocr_tests_enabled OFF)
endif()

if(_pp_ocr_tests_enabled AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/paddle_ocr_plugin_test.cc")
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
    test/paddle_ocr_plugin_test.cc
    ${PLUGIN_SOURCES}
  )
  apply_standard_settings(${TEST_RUNNER})
  target_compile_features(${TEST_RUNNER} PRIVATE cxx_std_17)
  target_include_directories(${TEST_RUNNER} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${PLUGIN_SHARED_DIR}"
    "${ONNXRUNTIME_INCLUDE_DIR}"
    "${OpenCV_INCLUDE_DIRS}"
  )
  target_link_libraries(${TEST_RUNNER} PRIVATE
    flutter
    PkgConfig::GTK
    ${ONNXRUNTIME_LINK_TARGET}
    ${OpenCV_LIBS}
    gtest_main gmock)

  include(GoogleTest)
  gtest_discover_tests(${TEST_RUNNER})
endif()
