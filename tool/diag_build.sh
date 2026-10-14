#!/usr/bin/env bash
# 构建并运行本地诊断工具（调参 / 回归确认用，不属于插件产物）。
#
# 用法：
#   tool/diag_build.sh diag    [模型目录] [图片路径]   # 各预设横向对比
#   tool/diag_build.sh probe   [模型目录] [图片路径]   # 单分支 × 增强矩阵（ROI 探针）
#   tool/diag_build.sh probe2  [模型目录] [图片路径]   # 条带 × 变换矩阵（找回「一舍」实验）
#   tool/diag_build.sh buildonly                      # 只编译
#
# 依赖：libopencv-dev、仓库内 linux/third_party/onnxruntime/（见
# tool/setup_linux_deps.sh）。源码在 tool/diag/，产物在 build/diag/。
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
ORT="$REPO/linux/third_party/onnxruntime"
OUT="$REPO/build/diag"
SRC_DIR="$REPO/tool/diag"
mkdir -p "$OUT"

if [ ! -e "$ORT/lib/libonnxruntime.so" ]; then
  echo "缺少 ONNX Runtime：先运行 tool/setup_linux_deps.sh" >&2
  exit 1
fi

ENGINE_SRCS=(
  "$REPO/cpp/ocr_engine.cpp"
  "$REPO/cpp/ocr_preprocess.cpp"
  "$REPO/cpp/ocr_options.cpp"
)
CXXFLAGS=(-std=c++17 -O1 -DPADDLE_OCR_DEBUG=0)
# 本机 /usr/local/lib 下的 sqlite3 缺符号，会让 OpenCV 依赖的 libgdal 链接失败。
# 这是环境问题而非插件问题，用下面两个开关绕开。
LDFLAGS=(-Wl,--unresolved-symbols=ignore-in-shared-libs)
INC=(-I"$REPO/cpp" -I"$ORT/include" $(pkg-config --cflags opencv4))
LIBS=(-lopencv_imgproc -lopencv_imgcodecs -lopencv_core
      -L"$ORT/lib" -l:libonnxruntime.so -lpthread -ldl)

build_one() {
  local name="$1"
  g++ "${CXXFLAGS[@]}" "${LDFLAGS[@]}" "$SRC_DIR/$name.cpp" \
      "${ENGINE_SRCS[@]}" "${INC[@]}" "${LIBS[@]}" -o "$OUT/$name"
  echo "built $OUT/$name"
}

what="${1:-both}"
shift || true

case "$what" in
  diag|probe|probe2|probe3|probe4|probe5)
    build_one "$what"
    ;;
  buildonly)
    build_one diag
    build_one probe
    build_one probe2
    build_one probe3
    build_one probe4
    build_one probe5
    exit 0
    ;;
  both)
    build_one diag
    build_one probe
    build_one probe2
    build_one probe3
    build_one probe4
    build_one probe5
    exit 0
    ;;
  *)
    echo "未知参数：$what（可用：diag | probe | probe2 | probe3 | both | buildonly）" >&2
    exit 2
    ;;
esac

export LD_PRELOAD="${LD_PRELOAD:-/usr/lib/x86_64-linux-gnu/libsqlite3.so.0}"
export LD_LIBRARY_PATH="$ORT/lib:${LD_LIBRARY_PATH:-}"

if [ "$what" = "diag" ] || [ "$what" = "probe" ] || [ "$what" = "probe2" ] || [ "$what" = "probe3" ] || [ "$what" = "probe4" ] || [ "$what" = "probe5" ]; then
  "$OUT/$what" "${1:-$REPO/model}" "${@:2}"
fi
