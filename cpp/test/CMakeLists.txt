# Standalone CMake project to build & run the shared C++ unit tests.
#
# This decouples the C++ tests from the Flutter tooling so that developers
# can run them with plain `cmake -S cpp/test -B build && cmake --build build
# && ctest --test-dir build`. It requires only:
#   * A C++17 compiler
#   * OpenCV 4.x (system package: libopencv-dev)
#   * ONNX Runtime (either system-installed or via -DONNXRUNTIME_ROOT)
#   * GoogleTest (fetched via FetchContent; requires network once, cached
#     in the build tree afterwards)
#
# The plugin's Linux/Windows Flutter bindings (paddle_ocr_plugin.cc/.cpp)
# are intentionally NOT compiled here—those are exercised by the platform
# CMake runners when `include_pp_ocr_tests` is enabled at the app level.
cmake_minimum_required(VERSION 3.14)
project(pp_ocr_cpp_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Locate the plugin's shared source directory (cpp/) relative to this file.
set(PLUGIN_CPP_DIR "${CMAKE_CURRENT_LIST_DIR}/..")

# ---------------------------------------------------------------------------
# ONNX Runtime (mirror the resolution order used by the plugin CMake files)
# ---------------------------------------------------------------------------
set(_bundled_onnx "${CMAKE_CURRENT_LIST_DIR}/../../linux/third_party/onnxruntime")
if(DEFINED ONNXRUNTIME_ROOT AND EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
  set(ORT_INC "${ONNXRUNTIME_ROOT}/include")
  set(ORT_LIB "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so")
  if(WIN32)
    set(ORT_LIB "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib")
  endif()
elseif(EXISTS "${_bundled_onnx}/include/onnxruntime_cxx_api.h")
  set(ORT_INC "${_bundled_onnx}/include")
  set(ORT_LIB "${_bundled_onnx}/lib/libonnxruntime.so")
  if(WIN32)
    set(ORT_LIB "${_bundled_onnx}/lib/onnxruntime.lib")
  endif()
else()
  find_path(ORT_INC onnxruntime_cxx_api.h
    PATHS /usr/include/onnxruntime /usr/local/include/onnxruntime
          /usr/include /usr/local/include)
  find_library(ORT_LIB onnxruntime
    PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu)
  if(NOT ORT_INC OR NOT ORT_LIB)
    message(FATAL_ERROR
      "ONNX Runtime not found for cpp tests. "
      "Pass -DONNXRUNTIME_ROOT=<path> or install the system package.")
  endif()
endif()

# ---------------------------------------------------------------------------
# OpenCV
# ---------------------------------------------------------------------------
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

# ---------------------------------------------------------------------------
# GoogleTest：优先用系统安装的（apt install libgtest-dev），
# 否则回退到 FetchContent（需能访问 github）。内网环境可以预
# 先 clone 一份到 cpp/third_party/googletest 并设 GIT_REPOSITORY。
# ---------------------------------------------------------------------------
find_package(GTest QUIET)
if(GTest_FOUND)
  message(STATUS "[pp_ocr-tests] Using system GoogleTest: ${GTest_VERSION}")
  set(_gtest_main_lib GTest::gtest_main)
  set(_gtest_lib      GTest::gtest)
  set(_gmock_lib      GTest::gmock)
else()
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../third_party/googletest/CMakeLists.txt")
    message(STATUS "[pp_ocr-tests] Using vendored googletest at cpp/third_party/googletest")
    set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST
        "${CMAKE_CURRENT_LIST_DIR}/../third_party/googletest")
  endif()
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/release-1.11.0.zip
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  set(_gtest_main_lib gtest_main)
  set(_gtest_lib      gtest)
  set(_gmock_lib      gmock)
endif()

enable_testing()

# ---------------------------------------------------------------------------
# Test runner
# ---------------------------------------------------------------------------
add_executable(pp_ocr_cpp_tests
  "${PLUGIN_CPP_DIR}/pp_ocr_ffi.cpp"
  "${PLUGIN_CPP_DIR}/ocr_engine.cpp"
  "${CMAKE_CURRENT_LIST_DIR}/cpp_shared_tests.cpp"
)

target_include_directories(pp_ocr_cpp_tests PRIVATE
  "${PLUGIN_CPP_DIR}"
  "${ORT_INC}"
  "${OpenCV_INCLUDE_DIRS}"
)

target_link_libraries(pp_ocr_cpp_tests PRIVATE
  ${_gtest_main_lib}
  ${_gmock_lib}
  "${ORT_LIB}"
  ${OpenCV_LIBS}
)

target_compile_features(pp_ocr_cpp_tests PRIVATE cxx_std_17)

include(GoogleTest)
gtest_discover_tests(pp_ocr_cpp_tests)
