# pp_ocr

English | [简体中文](README_zh.md)

A Flutter plugin for offline OCR (Optical Character Recognition) on Windows and Linux, powered by PaddleOCR's PP-OCRv6 models and ONNX Runtime.

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
| Linux    | ✅       |
| Android  | ❌       |
| iOS      | ❌       |
| macOS    | ❌       |

## Requirements

**Windows:**
- Flutter >= 3.0.0
- Windows 10 or later
- Visual Studio 2022 with CMake support

**Linux:**
- Flutter >= 3.0.0
- GTK 3 development headers: `sudo apt install libgtk-3-dev`
- CMake / clang toolchain: `sudo apt install cmake clang ninja-build`
- OpenCV development package: `sudo apt install libopencv-dev` (4.x recommended)
- ONNX Runtime: no official apt package on Ubuntu. Download the prebuilt
  tarball from [Microsoft's GitHub
  Releases](https://github.com/microsoft/onnxruntime/releases)
  (`onnxruntime-linux-x64-<ver>.tgz`), then either place its `include/`
  and `lib/` under `linux/third_party/onnxruntime/`, or pass
  `-DONNXRUNTIME_ROOT=<path>` via your `example/linux/CMakeLists.txt`.

### One-shot ONNX Runtime fetch (for networks that cannot reach GitHub)

If `github.com` times out (common in mainland China), this repo ships
`tool/setup_linux_deps.sh`. It downloads the equivalent `onnxruntime` wheel
from a PyPI mirror, extracts `libonnxruntime.so.<ver>` into
`linux/third_party/onnxruntime/`, and creates the `libonnxruntime.so` /
`libonnxruntime.so.1` SONAME symlinks the linker requires.

```bash
bash tool/setup_linux_deps.sh          # defaults to ORT 1.20.1
ORT_VERSION=1.17.0 bash tool/setup_linux_deps.sh
```

Then simply:
```bash
cd example && flutter run -d linux
```

### Crash after picking a file on Linux (Impeller / Mesa driver)

On some integrated GPUs — notably **Loongson GF** (PCI vendor `4c54`) — the bundled Mesa driver `gf_dri.so` has a null-pointer bug that Flutter's default **Impeller OpenGLESSDF** backend triggers when a GTK file dialog opens/closes. Symptom: the example's `pickImage()` returns the chosen path but the app crashes right after (`Lost connection to device`). Kernel log:

```
kernel: pp_ocr_example[...]: segfault at 2 in gf_dri.so
```

The plugin already mitigates this: `pickImage` prefers `GtkFileChooserNative`, which on systems with `xdg-desktop-portal` delegates the picker to a separate process (no GL in our process). If you still hit it, use one of these workarounds:

```bash
# Option A: force software rasterization (simplest)
LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe flutter run -d linux

# Option B: force X11 backend (helps on some Wayland compositors)
GDK_BACKEND=x11 flutter run -d linux

# Option C: bake the switch into the app. At the top of
# example/linux/runner/main.cc, before any GTK calls:
g_setenv("LIBGL_ALWAYS_SOFTWARE", "1", /*overwrite=*/FALSE);
```

To sanity-check the whole pipeline headlessly (no dialog, no mouse), the example
ships a self-test triggered by env vars:

```bash
PP_OCR_SELFTEST_IMAGE=/path/to/your.jpg \
PP_OCR_PICKIMAGE_FAKE_PATH=/path/to/your.jpg \
PP_OCR_SELFTEST_VIA_PICK=1 \
LIBGL_ALWAYS_SOFTWARE=1 \
flutter run -d linux
# Look for [SELFTEST] step 1→5 followed by ALL STEPS PASSED in the log.
```

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
  pp_ocr: ^0.2.1
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
