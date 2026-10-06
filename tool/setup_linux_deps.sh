#!/usr/bin/env bash
# 在 Linux 上为本插件准备内置的 ONNX Runtime。
#
# 用法：
#   bash tool/setup_linux_deps.sh            # 默认版本 1.20.1
#   ORT_VERSION=1.17.0 bash tool/setup_linux_deps.sh
#
# 为什么不用直连 GitHub Release：国内或部分网络下 github.com
# 会连接超时；PyPI 上的 onnxruntime wheel 走的是 pip 镜像，通常
# 更快且稳定。wheel 内包含 C/C++ 头文件所需的所有 .so（Linux 版本），
# 结构与 Microsoft 官方 tarball 中的 libonnxruntime.so.1.20.1 相同。
set -euo pipefail

ORT_VERSION="${ORT_VERSION:-1.20.1}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DST="${REPO_ROOT}/linux/third_party/onnxruntime"

mkdir -p "${DST}/include" "${DST}/lib"

# 1. 头文件：直接复用 windows/third_party/onnxruntime/include 的
#   （ORT 的 C/C++ 头文件跨平台一致）。若 Windows 端不存在，
#   则从 wheel 内的 onnxruntime/include 拷贝。
if [ -d "${REPO_ROOT}/windows/third_party/onnxruntime/include" ]; then
  cp -r "${REPO_ROOT}/windows/third_party/onnxruntime/include/." \
        "${DST}/include/"
  echo "[setup] Headers copied from windows/third_party."
fi

# 2. 下载 wheel 并抽取 .so
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

echo "[setup] Downloading onnxruntime==${ORT_VERSION} wheel via pip..."
python3 -m pip download "onnxruntime==${ORT_VERSION}" --no-deps -d "${TMP}"

WHL="$(ls "${TMP}"/onnxruntime-*.whl | head -n1)"
echo "[setup] Extracting ${WHL}..."
python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" \
         "${WHL}" "${TMP}/extract"

# 3. 拷贝 libonnxruntime.so.${ORT_VERSION} 与 provider 插件
CAPI="${TMP}/extract/onnxruntime/capi"
cp "${CAPI}"/libonnxruntime.so.* "${DST}/lib/"
cp "${CAPI}"/libonnxruntime_providers_shared.so "${DST}/lib/" 2>/dev/null || true

# 4. 建立常见 SONAME 链接：
#      libonnxruntime.so.1 -> libonnxruntime.so.${ORT_VERSION}
#      libonnxruntime.so   -> libonnxruntime.so.${ORT_VERSION}
# 前者是运行时 DT_NEEDED 的名字（ONNX Runtime 的 SONAME），后者
# 供 CMake 的 find_library / 直接 -lonnxruntime 使用。
LINK_TARGET="$(basename "$(ls "${DST}"/lib/libonnxruntime.so.${ORT_VERSION})")"
( cd "${DST}/lib" \
  && ln -sf "${LINK_TARGET}" libonnxruntime.so.1 \
  && ln -sf "${LINK_TARGET}" libonnxruntime.so )

echo "[setup] Done. Layout:"
ls -la "${DST}/lib"
echo
echo "现在可以在 example 目录运行: flutter build linux  或  flutter run -d linux"
