# pp_ocr

[English](README.md) | 简体中文

一个基于 PaddleOCR PP-OCRv6 模型和 ONNX Runtime 的 Windows/Linux 离线 OCR（光学字符识别）Flutter 插件。

## 功能特性

- **离线 OCR** — 无需网络连接，所有推理在本地运行
- **双后端架构** — 默认使用 `dart:ffi` 直接调用原生代码（低延迟），自动回退到 `MethodChannel`
- **文本检测** — 基于 DB（可微分二值化）的文本检测
- **文本识别** — 基于 CRNN + CTC 的文本识别，支持中文、英文及 18,000+ 字符
- **Unicode 路径支持** — 正确处理 Windows 上的非 ASCII 文件路径（中文、日文等）
- **内置 ONNX Runtime** — ONNX Runtime 已打包，无需下载；OpenCV 通过 `find_package` 解析（设置 `OpenCV_DIR`）
- **检测框可视化** — 返回检测框坐标、识别文本和置信度

## 平台支持

| 平台    | 支持   |
|---------|--------|
| Windows | ✅      |
| Linux   | ✅      |
| Android | ❌      |
| iOS     | ❌      |
| macOS   | ❌      |

## 环境要求

**Windows：**
- Flutter >= 3.0.0
- Windows 10 及以上
- Visual Studio 2022（含 CMake 支持）

**Linux：**
- Flutter >= 3.0.0
- GTK 3 开发头文件：`sudo apt install libgtk-3-dev`
- CMake、clang 工具链（`sudo apt install cmake clang ninja-build`）
- OpenCV 开发包：`sudo apt install libopencv-dev`（建议 4.x）
- ONNX Runtime：Ubuntu 上无官方 apt 包，推荐从 [Microsoft
  GitHub Release](https://github.com/microsoft/onnxruntime/releases)
  下载 `onnxruntime-linux-x64-<ver>.tgz`，解压后将 `include/` 与
  `lib/` 放到 `linux/third_party/onnxruntime/`，或通过
  `-DONNXRUNTIME_ROOT=<path>` 传递（写入 `example/linux/CMakeLists.txt`
  或 `flutter build linux --config=cmake_args=...`）。

### 一键拉取 ONNX Runtime（适合无法直连 GitHub 的网络）

GitHub Release 在国内容易超时，本仓库提供 `tool/setup_linux_deps.sh`：
它从 pip 镜像拉 `onnxruntime==<ver>.whl`（内部同样包含 Linux 的
`libonnxruntime.so.<ver>`），拷贝到 `linux/third_party/onnxruntime/`。

```bash
bash tool/setup_linux_deps.sh          # 默认 ORT 1.20.1
ORT_VERSION=1.17.0 bash tool/setup_linux_deps.sh
```

完成后直接：
```bash
cd example && flutter run -d linux
```

### Linux 上选完图片后闪退（Impeller / Mesa 驱动已知问题）

部分国产集显（如 **Loongson GF**，PCI vendor `4c54`）自带的 Mesa 驱动 `gf_dri.so`，在 Flutter Linux 默认的 **Impeller OpenGLESSDF** 后端搭配 GTK 文件对话框开启/关闭时存在已知空指针，表现为 example 中 `pickImage` 选完图一瞬间 `Lost connection to device`，内核日志能看到：

```
kernel: pp_ocr_example[...]: segfault at 2 in gf_dri.so
```

插件侧已尽量回避（`pickImage` 优先使用 `GtkFileChooserNative`，在有 `xdg-desktop-portal` 的环境下会把选文件交给独立进程，不走本进程 GL）。仍重现时，任选一个应急方案：

```bash
# 方案 A：强制软件光栅（最简单）
LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe flutter run -d linux

# 方案 B：强制走 X11，部分 Wayland 合成器下可缓解
GDK_BACKEND=x11 flutter run -d linux

# 方案 C：发布包时内置开关。在 example/linux/runner/main.cc 开头：
g_setenv("LIBGL_ALWAYS_SOFTWARE", "1", /*overwrite=*/FALSE);
```

确认无头链路健康可以直接跑自带的 self-test（不会弹任何 dialog，不需要鼠标）：

```bash
PP_OCR_SELFTEST_IMAGE=/path/to/your.jpg \
PP_OCR_PICKIMAGE_FAKE_PATH=/path/to/your.jpg \
PP_OCR_SELFTEST_VIA_PICK=1 \
LIBGL_ALWAYS_SOFTWARE=1 \
flutter run -d linux
# 日志中会依次看到 [SELFTEST] step 1→ 5 以及 ALL STEPS PASSED
```

> **注意：** ONNX Runtime 1.20.1 已**内置**在本插件中
> （`windows/third_party/`），无需下载或手动配置。
>
> 内置文件包括：
> - `onnxruntime.dll` / `onnxruntime.lib` / 头文件 — ONNX Runtime 推理引擎
>
> OpenCV 在 Windows 上**不再内置**（原先内置的 opencv_world490 二进制已移除，
> 所有构建统一走 `find_package(OpenCV)`）。请通过 `-DOpenCV_DIR=<OpenCVConfig.cmake
> 所在路径>` 或 `OpenCV_DIR` 环境变量指向你的 OpenCV 安装。交叉编译场景建议用
> 同工具链静态编译的 OpenCV（静态链接，产物无需携带 OpenCV DLL）。Linux 上
> 安装 `libopencv-dev` 或同样设置 `OpenCV_DIR` 即可。

## 安装

在 `pubspec.yaml` 中添加依赖：

```yaml
dependencies:
  pp_ocr: ^0.2.1
```

## 模型文件

下载 PP-OCRv6 ONNX 模型，放置在 `model/` 目录下：

```
model/
├── det.onnx            # 文本检测模型
├── inference.onnx       # 文本识别模型
└── ppocr_v6_dict.txt    # 字符字典（18,708 个条目）
```

模型可从 [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR) 获取，
并使用 Paddle2ONNX 转换为 ONNX 格式。

## 使用方法

```dart
import 'package:pp_ocr/pp_ocr.dart';

final ocr = PaddleOcr();

// 使用模型路径初始化
await ocr.initialize(
  detModelPath: 'path/to/det.onnx',
  recModelPath: 'path/to/inference.onnx',
  dictPath: 'path/to/ppocr_v6_dict.txt',
);

// 从图片文件识别文字
final results = await ocr.recognizeImage('path/to/image.png');

for (final result in results) {
  print('文本: ${result.text}');
  print('置信度: ${result.confidence}');
  print('检测框: ${result.box}');
}

// 使用完毕后释放原生资源
ocr.dispose();
```

完整示例请参考 [example 应用](example/)，包含图片选择器和检测框可视化叠加。

## 架构说明

本插件支持两种后端：

### FFI（默认）
- C++ 引擎编译为 DLL，导出 C API（`pp_ocr_ffi.h`）
- Dart 通过 `dart:ffi` 直接调用原生函数
- 延迟更低，无序列化开销
- 如果 DLL 未找到，自动回退到 MethodChannel

### MethodChannel（回退）
- 传统 Flutter 插件架构，通过 `MethodChannel('paddle_ocr')` 通信
- 在 FFI DLL 不可用时使用

```
┌──────────────────────────────────────────────┐
│                Dart (pp_ocr.dart)            │
│                        │                     │
│         ┌──────────────┴──────────────┐      │
│         │  PaddleOcrPlatform          │      │
│         │  (接口)                     │      │
│         └──────┬──────────────┬───────┘      │
│                │              │              │
│     ┌──────────▼──┐  ┌───────▼────────┐     │
│     │ FfiPaddleOcr│  │MethodChannel   │     │
│     │ (默认)      │  │PaddleOcr       │     │
│     │ dart:ffi    │  │ (回退)         │     │
│     └──────┬──────┘  └──────┬─────────┘     │
│            │                │               │
└────────────┼────────────────┼───────────────┘
             │                │
     ┌───────▼───────┐ ┌──────▼──────────┐
     │ pp_ocr_ffi.h  │ │ paddle_ocr_     │
     │ pp_ocr_ffi.cpp│ │ plugin.cpp      │
     │ (C API)       │ │ (MethodChannel) │
     └───────┬───────┘ └──────┬──────────┘
             │                │
             └───────┬────────┘
                     │
             ┌───────▼───────┐
             │  ocr_engine   │
             │  .cpp/.h      │
             │  (OCR 核心)   │
             └───────┬───────┘
                     │
          ┌──────────┴──────────┐
          │ONNX Runtime+ OpenCV │
          │(内置)               │
          └─────────────────────┘
```

## 许可证

本项目基于 [MIT 许可证](LICENSE) 开源。

本项目使用了以下第三方组件：
- [OpenCV](https://opencv.org/)（Apache License 2.0）
- [PaddleOCR PP-OCRv6](https://github.com/PaddlePaddle/PaddleOCR) 模型（Apache License 2.0）
- [ONNX Runtime](https://onnxruntime.ai/)（MIT 许可证）

详见 [NOTICE](NOTICE)。
