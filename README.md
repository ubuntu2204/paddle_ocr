# paddle_ocr

A Flutter plugin for offline OCR (Optical Character Recognition) on Windows, powered by PaddleOCR's PP-OCRv6 models and ONNX Runtime.

## Features

- **Offline OCR** — No internet connection required. All inference runs locally.
- **Text Detection** — DB (Differentiable Binarization) based text detection.
- **Text Recognition** — CRNN + CTC based text recognition with support for Chinese, English, and 18,000+ characters.
- **Unicode Path Support** — Correctly handles non-ASCII file paths (Chinese, Japanese, etc.) on Windows.
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
- ONNX Runtime 1.20.1 (auto-downloaded during build)
- OpenCV 4.9.0 (auto-downloaded or pre-built)

## Installation

Add this to your `pubspec.yaml`:

```yaml
dependencies:
  paddle_ocr:
    git:
      url: https://github.com/yourusername/paddle_ocr.git
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
import 'package:paddle_ocr/paddle_ocr.dart';

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
```

See the [example app](example/) for a complete demo with image picker and visual box overlay.

## Architecture

The plugin consists of:

- **Dart layer** (`lib/`) — Platform interface and method channel implementation
- **C++ layer** (`windows/`) — Native OCR engine using ONNX Runtime and OpenCV
  - `ocr_engine.cpp` — Text detection (DB) and recognition (CRNN+CTC) pipeline
  - `paddle_ocr_plugin.cpp` — Flutter method channel handler
  - `debug_utils.h` — UTF-8 validation and debug logging utilities

## License

See [LICENSE](LICENSE).
