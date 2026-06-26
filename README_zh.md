# pp_ocr

[English](README.md) | 简体中文

一个基于 PaddleOCR PP-OCRv6 模型和 ONNX Runtime 的 Windows 离线 OCR（光学字符识别）Flutter 插件。

## 功能特性

- **离线 OCR** — 无需网络连接，所有推理在本地运行
- **文本检测** — 基于 DB（可微分二值化）的文本检测
- **文本识别** — 基于 CRNN + CTC 的文本识别，支持中文、英文及 18,000+ 字符
- **Unicode 路径支持** — 正确处理 Windows 上的非 ASCII 文件路径（中文、日文等）
- **检测框可视化** — 返回检测框坐标、识别文本和置信度

## 平台支持

| 平台    | 支持   |
|---------|--------|
| Windows | ✅      |
| Android | ❌      |
| iOS     | ❌      |
| macOS   | ❌      |
| Linux   | ❌      |

## 环境要求

- Flutter >= 3.0.0
- Windows 10 及以上
- Visual Studio 2022（含 CMake 支持）

> **注意：** ONNX Runtime 1.20.1 和 OpenCV 4.9.0 已**内置**在本插件中
> （`windows/third_party/`），无需下载或手动配置。
>
> 内置文件包括：
> - `onnxruntime.dll` / `onnxruntime.lib` / 头文件 — ONNX Runtime 推理引擎
> - `opencv_world490.dll` / `opencv_world490.lib` / `opencv_world490d.lib` / 头文件 — OpenCV 图像处理
>
> 如果运行时遇到"找不到 `opencv_world490d.dll`"的错误，说明你的应用在 Debug 模式下运行，
> 但 debug DLL 未找到。由于 debug DLL（124 MB）超过 GitHub 文件大小限制，只内置了 release 版
> `opencv_world490.dll`。**Debug 构建在运行时使用 release DLL** — 这是安全的，可以正常工作。
> 只需确保 `opencv_world490.dll` 已复制到应用输出目录（这通过 CMake `bundled_libraries` 自动完成）。

## 安装

在 `pubspec.yaml` 中添加依赖：

```yaml
dependencies:
  pp_ocr: ^0.1.3
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
```

完整示例请参考 [example 应用](example/)，包含图片选择器和检测框可视化叠加。

## 架构说明

本插件由以下部分组成：

- **Dart 层** (`lib/`) — 平台接口和 Method Channel 实现
- **C++ 层** (`windows/`) — 使用 ONNX Runtime 和 OpenCV 的原生 OCR 引擎
  - `ocr_engine.cpp` — 文本检测（DB）和识别（CRNN+CTC）流水线
  - `paddle_ocr_plugin.cpp` — Flutter Method Channel 处理器
  - `debug_utils.h` — UTF-8 验证和调试日志工具

## 许可证

本项目基于 [MIT 许可证](LICENSE) 开源。

本项目使用了以下第三方组件：
- [OpenCV](https://opencv.org/)（Apache License 2.0）
- [PaddleOCR PP-OCRv6](https://github.com/PaddlePaddle/PaddleOCR) 模型（Apache License 2.0）
- [ONNX Runtime](https://onnxruntime.ai/)（MIT 许可证）

详见 [NOTICE](NOTICE)。
