#!/usr/bin/env bash
# 一键运行本仓库所有自动化测试。
#
# 覆盖：
#   1. Dart 静态分析（flutter analyze）
#   2. Dart 单元测试（flutter test）
#   3. C++ 共享单元测试（cpp/test/cpp_shared_tests.cpp）
#      —— 通过独立 CMake 项目构建，不依赖 flutter
#   4. （可选）Linux flutter build 冒烟 —— 传 --with-build 时启用
#
# 依赖：
#   - Flutter SDK、CMake、C++17 编译器
#   - OpenCV 4.x 开发头（apt install libopencv-dev）
#   - ONNX Runtime：先运行 tool/setup_linux_deps.sh 拉取到
#     linux/third_party/onnxruntime/
#   - GoogleTest：优先系统包（apt install libgtest-dev）；否则
#     CMake 会用 FetchContent 从 github 拉一次。
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

WITH_BUILD="${1:-}"

GREEN=$'\033[32m'; RED=$'\033[31m'; CYAN=$'\033[36m'; RESET=$'\033[0m'
section() { echo; echo "${CYAN}==> $*${RESET}"; }
ok()      { echo "${GREEN}✓ $*${RESET}"; }
bad()     { echo "${RED}✗ $*${RESET}"; }

section "flutter analyze"
if flutter analyze --no-fatal-infos 2>&1 | tail -5; then
  ok "analyze"
else
  bad "analyze"
fi

section "flutter test (Dart unit tests)"
flutter test 2>&1 | tail -5
ok "dart tests"

section "C++ unit tests"
if [ ! -d linux/third_party/onnxruntime ]; then
  echo "  hint: ONNX Runtime 未就位，先运行 tool/setup_linux_deps.sh"
fi
mkdir -p build/cpp_tests
if cmake -S cpp/test -B build/cpp_tests > build/cpp_tests/configure.log 2>&1; then
  ok "cmake configure"
  if cmake --build build/cpp_tests -j > build/cpp_tests/build.log 2>&1; then
    ok "cmake build"
    if ctest --test-dir build/cpp_tests --output-on-failure; then
      ok "ctest"
    else
      bad "ctest"
      exit 1
    fi
  else
    bad "cmake build (see build/cpp_tests/build.log)"
    tail -30 build/cpp_tests/build.log
    exit 1
  fi
else
  bad "cmake configure (see build/cpp_tests/configure.log)"
  tail -30 build/cpp_tests/configure.log
  exit 1
fi

if [ "$WITH_BUILD" = "--with-build" ]; then
  section "flutter build linux (smoke)"
  cd example
  rm -rf build/linux
  if flutter build linux > /tmp/flutter_build.log 2>&1; then
    ok "flutter build linux"
    tail -3 /tmp/flutter_build.log
  else
    bad "flutter build linux"
    tail -30 /tmp/flutter_build.log
    exit 1
  fi
  cd ..
fi

echo
ok "All tests passed."
