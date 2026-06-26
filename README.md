# pp_ocr

English | [简体中文](README_zh.md)

A Flutter plugin for offline OCR (Optical Character Recognition) on Windows, powered by PaddleOCR's PP-OCRv6 models and ONNX Runtime.

## Features

- **Offline OCR** — No internet connection required. All inference runs locally.
- **Dual Backend** — Uses `dart:ffi` by default for low-latency direct native calls, with automatic fallback to `MethodChannel`.
- **Text Detection** — DB (Differentiable Binarization) based text detection.
- **Text Recognition** — CRNN + CTC based text recognition with support for Chinese, English, and 18,000+ characters.
- **Unicode Path Support** — Correctly handles non-ASCII file paths (Chinese, Japanese, etc.) on Windows.
- **Bundled Dependencies** — ONNX Runtime and OpenCV are included, no download required.
- **Bounding Box Overlay** — Returns detection boxes with recognized text and confidence scores.

## Platform Support

| Platform | Support |
|----------|---------|
| Windows  | ✅       |
| Android  | ❌       |
| iOS      | ❌       |
| macOS    | ❌       |
| Linux    | ❌       |

## Requirements

- Flutter >= 3.0.0
- Windows 10 or later
- Visual Studio 2022 with CMake support

> **Note:** ONNX Runtime 1.20.1 and OpenCV 4.9.0 are **bundled** with this plugin
> (`windows/third_party/`). No download or manual setup is required.
>
> The bundled files include:
> - `onnxruntime.dll` / `onnxruntime.lib` / headers — ONNX Runtime inference engine
> - `opencv_world490.dll` / `opencv_world490.lib` / `opencv_world490d.lib` / headers — OpenCV image processing
>
> If you encounter a "missing `opencv_world490d.dll`" error at runtime, it means
> your app is running in Debug mode and the debug DLL was not found. Since the debug
> DLL (124 MB) exceeds GitHub's file size limit, only the release `opencv_world490.dll`
> is bundled. **Debug builds use the release DLL at runtime** — this is safe and works
> correctly. Simply ensure `opencv_world490.dll` is copied to your app's output directory
> (this happens automatically via CMake `bundled_libraries`).

## Installation

Add this to your `pubspec.yaml`:

```yaml
dependencies:
  pp_ocr: ^0.2.0
```

## Model Files

Download the PP-OCRv6 ONNX models and place them in a `model/` directory:

```
model/
├── det.onnx            # Text detection model
├── inference.onnx       # Text recognition model
└── ppocr_v6_dict.txt    # Character dictionary (18,708 entries)
```

Models can be obtained from [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR)
and converted to ONNX format using Paddle2ONNX.

## Usage

```dart
import 'package:pp_ocr/pp_ocr.dart';

final ocr = PaddleOcr();

// Initialize with model paths
await ocr.initialize(
  detModelPath: 'path/to/det.onnx',
  recModelPath: 'path/to/inference.onnx',
  dictPath: 'path/to/ppocr_v6_dict.txt',
);

// Recognize text from an image file
final results = await ocr.recognizeImage('path/to/image.png');

for (final result in results) {
  print('Text: ${result.text}');
  print('Confidence: ${result.confidence}');
  print('Box: ${result.box}');
}

// Release native resources when done
ocr.dispose();
```

See the [example app](example/) for a complete demo with image picker and visual box overlay.

## Architecture

The plugin supports two backends:

### FFI (Default)
- C++ engine compiled as DLL with C API (`pp_ocr_ffi.h`)
- Dart calls native functions directly via `dart:ffi`
- Lower latency, no serialization overhead
- Automatic fallback to MethodChannel if DLL not found

### MethodChannel (Fallback)
- Traditional Flutter plugin architecture via `MethodChannel('paddle_ocr')`
- Used when FFI DLL is unavailable

```
┌──────────────────────────────────────────────┐
│                Dart (pp_ocr.dart)            │
│                        │                     │
│         ┌──────────────┴──────────────┐      │
│         │  PaddleOcrPlatform          │      │
│         │  (interface)                │      │
│         └──────┬──────────────┬───────┘      │
│                │              │              │
│     ┌──────────▼──┐  ┌───────▼────────┐     │
│     │ FfiPaddleOcr│  │MethodChannel   │     │
│     │ (default)   │  │PaddleOcr       │     │
│     │ dart:ffi    │  │ (fallback)     │     │
│     └──────┬──────┘  └───────┬────────┘     │
│            │                 │              │
└────────────┼─────────────────┼──────────────┘
             │                 │
     ┌───────▼───────┐  ┌──────▼──────────┐
     │ pp_ocr_ffi.h  │  │ paddle_ocr_     │
     │ pp_ocr_ffi.cpp│  │ plugin.cpp      │
     │ (C API)       │  │ (MethodChannel) │
     └───────┬───────┘  └──────┬──────────┘
             │                 │
             └────────┬────────┘
                      │
              ┌───────▼───────┐
              │  ocr_engine   │
              │  .cpp/.h      │
              │  (OCR core)   │
              └───────┬───────┘
                      │
           ┌──────────┴──────────┐
           │ONNX Runtime+ OpenCV │
           │(bundled)            │
           └─────────────────────┘
```

## License

This project is licensed under the [MIT License](LICENSE).

This project uses the following third-party components:
- [OpenCV](https://opencv.org/) (Apache License 2.0)
- [PaddleOCR PP-OCRv6](https://github.com/PaddlePaddle/PaddleOCR) models (Apache License 2.0)
- [ONNX Runtime](https://onnxruntime.ai/) (MIT License)

See [NOTICE](NOTICE) for details.
