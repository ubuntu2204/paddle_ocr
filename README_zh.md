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
  pp_ocr: ^0.4.0
```

## 模型文件

模型文件**不随** pub 包 / git 仓库分发（共约 77MB，过大；插件本身不依赖
它们——模型路径由你在 `initialize()` 时传入）。

下载 PP-OCRv6 ONNX 模型，放置在 `model/` 目录下：

```
model/
├── det.onnx            # 文本检测模型
├── inference.onnx       # 文本识别模型
└── ppocr_v6_dict.txt    # 字符字典（18,708 个条目）
```

- **已转换好的副本**：从本仓库的
  [GitHub Releases](https://github.com/ubuntu2204/paddle_ocr/releases)
  下载（`model.zip` 附件）。
- **自行转换**：模型可从 [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR)
  获取，并使用 Paddle2ONNX 转换为 ONNX 格式。

> `example/` 会自动探测仓库根目录旁（或可执行文件旁）的 `model/`——
> 本地开发布局不变。

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

完整示例请参考 [example 应用](example/)，包含图片选择器、预设选择器和检测框可视化叠加。

### 识别结果不全（白字压复杂背景）

当一行字是**压在杂乱背景上的白字**（监控叠加字幕、路牌、低对比扫描件）时，
单路检测容易把它碎裂成几段，并丢头丢尾。此时传入预处理选项改走多分支召回：

```dart
// 一行搞定：按场景选预设
final results = await ocr.recognizeImage(
  'path/to/camera_frame.png',
  options: OcrOptions.preset(OcrPreset.surveillance),
);

// 或者 App 启动时配一次，之后不带 options 的调用全部继承
await ocr.setDefaultOptions(OcrOptions.preset(OcrPreset.surveillance));

// 只识别某个矩形内（结果坐标仍以原图为基准）
final band = await ocr.recognizeImage(
  'path/to/frame.png',
  options: OcrOptions.preset(OcrPreset.surveillance).withRoi(700, 0, 350, 60),
);
```

可用预设：`standard`（保持旧行为不变）、`document`、`lowContrast`、`surveillance`、
`smallText`、`maxRecall`。

预设只是个**起点** —— 读回来再只改你关心的字段：

```dart
final opts = OcrOptions.preset(OcrPreset.surveillance)
    .copyWith(detThresh: 0.22, branches: {OcrBranch.raw, OcrBranch.whiteText});
```

开启多分支后，管道实际做了四件事：

1. 每一路分支先做预处理（二值化 / CLAHE / 放大 / 白色字幕提取 / 放宽阈值）；
2. 各路分别检测，再跨分支合并去重；
3. 可选把同一行的碎框拼回整行；
4. 每个区域在所有候选图上各识别一次，取最优结果。

第 4 步很关键：PP-OCR 的 CTC 置信度极易饱和（漏了 3 个字仍报 `1.0`），所
以“最优”是按「结果估算宽度与区域长宽比的契合度」打分——它能同时惩罚“读少了”
和“多插字读油了”。二值化类分支**只用于检测**，拿它们的像素去喂识别器
只会得到大量乱码。

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
