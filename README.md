# pp_ocr

English | [简体中文](README_zh.md)

A Flutter plugin for offline OCR (Optical Character Recognition) on Windows and Linux, powered by PaddleOCR's PP-OCRv6 models and ONNX Runtime.

## Features

- **Offline OCR** — No internet connection required. All inference runs locally.
- **Dual Backend** — Uses `dart:ffi` by default for low-latency direct native calls, with automatic fallback to `MethodChannel`.
- **Text Detection** — DB (Differentiable Binarization) based text detection.
- **Text Recognition** — CRNN + CTC based text recognition with support for Chinese, English, and 18,000+ characters.
- **Unicode Path Support** — Correctly handles non-ASCII file paths (Chinese, Japanese, etc.) on Windows.
- **Bundled ONNX Runtime** — ONNX Runtime is included, no download required. OpenCV is resolved via `find_package` (set `OpenCV_DIR`).
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

> **Note:** ONNX Runtime 1.20.1 is **bundled** with this plugin
> (`windows/third_party/`). No download or manual setup is required.
>
> The bundled files include:
> - `onnxruntime.dll` / `onnxruntime.lib` / headers — ONNX Runtime inference engine
>
> OpenCV is **not** bundled on Windows anymore (the former bundled
> `opencv_world490` binaries were removed — every build now goes through
> `find_package(OpenCV)`). Point CMake at your OpenCV installation via
> `-DOpenCV_DIR=<path-to-OpenCVConfig.cmake>` or the `OpenCV_DIR` environment
> variable. For cross-compile scenarios, a static OpenCV built with the same
> toolchain is recommended (it links statically, so no DLL ships with the
> app). On Linux, install `libopencv-dev` or set `OpenCV_DIR` likewise.

## Installation

Add this to your `pubspec.yaml`:

```yaml
dependencies:
  pp_ocr: ^0.4.0
```

## Model Files

Model files are **not** included in the pub package or the git repository
(they total ~77 MB — too large to ship; the plugin itself needs none of them,
since you pass model paths to `initialize()` yourself).

Download the PP-OCRv6 ONNX models and place them in a `model/` directory:

```
model/
├── det.onnx            # Text detection model
├── inference.onnx       # Text recognition model
└── ppocr_v6_dict.txt    # Character dictionary (18,708 entries)
```

- **Pre-converted copies**: grab them from this repo's
  [GitHub Releases](https://github.com/ubuntu2204/paddle_ocr/releases)
  (`model.zip` attachment).
- **Convert yourself**: models can be obtained from
  [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR) and converted to
  ONNX format using Paddle2ONNX.

> The bundled `example/` app auto-detects `model/` next to the repo root (or
> beside the executable) — the local dev layout stays unchanged.

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

### Recognizing text that comes out incomplete

If a line is rendered as **white text over a busy background** (surveillance overlay
captions, road signs, low-contrast scans), the single-pass detector tends to break it
into fragments and drop the head or tail of the line. Pass preprocessing options to
run multi-branch recall instead:

```dart
// One-liner: pick the scenario preset
final results = await ocr.recognizeImage(
  'path/to/camera_frame.png',
  options: OcrOptions.preset(OcrPreset.surveillance),
);

// Or set it once at app start; every call without `options:` inherits it
await ocr.setDefaultOptions(OcrOptions.preset(OcrPreset.surveillance));

// Recognize only inside a region (coordinates stay in original-image space)
final band = await ocr.recognizeImage(
  'path/to/frame.png',
  options: OcrOptions.preset(OcrPreset.surveillance).withRoi(700, 0, 350, 60),
);
```

Presets: `standard` (unchanged legacy behaviour), `document`, `lowContrast`,
`surveillance`, `smallText`, `maxRecall`.

Each preset is a *starting point* — read it back and override only what you care about:

```dart
final opts = OcrOptions.preset(OcrPreset.surveillance)
    .copyWith(detThresh: 0.22, branches: {OcrBranch.raw, OcrBranch.whiteText});
```

What the pipeline does when several branches are enabled:

1. preprocess the image once per branch (binarize / CLAHE / upscale / white-caption
   extraction / looser thresholds);
2. detect on each, then merge duplicate regions across branches;
3. optionally stitch collinear fragments back into a full line;
4. recognize each region on every candidate image and keep the best reading.

Step 4 matters: PP-OCR's CTC confidence saturates (a reading missing three characters
still reports `1.0`), so "best" is scored by how well the result's estimated width fits
the region's aspect ratio — which penalises both under-reading and padded garbage.
Binarised branch images are used for **detection only**; feeding them to the recognizer
produces mostly gibberish.

### Injecting your own preprocessing (plugin interface)

The built-in branches are fixed. If your app has its own image processing
(super-resolution, colour-domain extraction, dehazing, …), implement
`OcrPreprocessor` and hand the variants to `runPipeline` — detection, region
merging and best-variant recognition are all shared with the engine:

```dart
final results = await ocr.runPipeline(imageBytes, [
  upscalePreprocessor(times: 3),           // built-in 3x upscale (native OpenCV)
  whiteTextPreprocessor(),                 // built-in white-caption extraction
  CustomTransformPreprocessor(             // your own Dart code
    (bytes) => mySuperResolutionModel(bytes),
    name: 'superres',
    scale: 2.0,                            // variant scale, for box mapping
  ),
], options: OcrOptions.preset(OcrPreset.surveillance));
```

Pipeline flow: each variant is detected → boxes are merged into recognition
windows → every window is recognized on the base image plus each `recSource`
variant, keeping the best reading. Binarised variants (`recSource: false`)
contribute detection boxes only.

The same four steps are exposed as plain C API atoms for non-Flutter hosts:
`pp_ocr_preprocess_bytes`, `pp_ocr_detect_bytes`, `pp_ocr_merge_boxes`,
`pp_ocr_recognize_box_bytes` (see `cpp/pp_ocr_ffi.h`).

See the [example app](example/) for a complete demo with image picker, preset
selector and visual box overlay.

## Architecture

The recognition path is **FFI only** — pushing whole images through a
MethodChannel costs a serialization round-trip per call, so no fallback exists.
The native library failing to load raises a `StateError` immediately instead of
silently degrading. A small `PluginChannelPaddleOcr` remains for the two things
FFI cannot do: the native file picker and platform version queries.

### FFI
- C++ engine compiled as DLL with C API (`pp_ocr_ffi.h`)
- Dart calls native functions directly via `dart:ffi`
- Lower latency, no serialization overhead

### MethodChannel (picker / platform info only)
- Traditional Flutter plugin architecture via `MethodChannel('paddle_ocr')`
- Used by `pickImage()` (native file dialog) and `getPlatformVersion()` only

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
