/// Flutter plugin for offline OCR using PP-OCRv6 with ONNX Runtime.
///
/// This plugin provides text detection and recognition capabilities
/// using PaddleOCR PP-OCRv6 models, running entirely offline on Windows.
///
/// The plugin supports two backends:
/// - **FFI** (default): Direct native calls via `dart:ffi`, lower latency
/// - **MethodChannel** (fallback): Used when FFI DLL is not available
///
/// ## Quick Start
///
/// ```dart
/// import 'package:pp_ocr/pp_ocr.dart';
///
/// final ocr = PaddleOcr();
///
/// // Initialize with model paths
/// await ocr.initialize(
///   detModelPath: r'C:\models\det.onnx',
///   recModelPath: r'C:\models\inference.onnx',
///   dictPath: r'C:\models\ppocr_v6_dict.txt',
/// );
///
/// // Recognize text from image
/// final results = await ocr.recognizeImage(r'C:\images\test.png');
/// for (final r in results) {
///   print('${r.text} (${r.confidence})');
/// }
///
/// // Don't forget to dispose when done
/// ocr.dispose();
/// ```
library;

export 'src/ocr_result.dart';
export 'paddle_ocr_platform_interface.dart' show PaddleOcrPlatform;

import 'dart:typed_data';

import 'paddle_ocr_platform_interface.dart';
import 'pp_ocr_ffi.dart';
import 'src/ocr_result.dart';

/// Main entry point for the PP-OCR plugin.
///
/// Provides offline text detection and recognition using PP-OCRv6 models
/// powered by ONNX Runtime C++ inference engine.
///
/// By default, uses FFI (dart:ffi) for direct native calls with automatic
/// fallback to MethodChannel if the FFI DLL is not available.
class PaddleOcr {
  PaddleOcrPlatform? _platform;

  /// Get the platform implementation.
  PaddleOcrPlatform get _impl => _platform ??= PaddleOcrPlatform.instance;

  /// Initialize the OCR engine.
  ///
  /// Must be called before any recognition methods.
  ///
  /// [detModelPath]: Path to the text detection ONNX model file.
  /// [recModelPath]: Path to the text recognition ONNX model file.
  /// [dictPath]: Path to the character dictionary text file.
  ///
  /// Returns `true` if initialization succeeded.
  Future<bool> initialize({
    required String detModelPath,
    required String recModelPath,
    required String dictPath,
  }) {
    return _impl.initialize(
      detModelPath: detModelPath,
      recModelPath: recModelPath,
      dictPath: dictPath,
    );
  }

  /// Recognize text from an image file.
  ///
  /// [imagePath]: Absolute path to the image file (png, jpg, bmp, tiff).
  ///
  /// Returns a list of [OcrResult], one per detected text region.
  Future<List<OcrResult>> recognizeImage(String imagePath) {
    return _impl.recognizeImage(imagePath);
  }

  /// Recognize text from image byte data.
  ///
  /// [imageBytes]: Raw bytes of the image file.
  ///
  /// Returns a list of [OcrResult], one per detected text region.
  Future<List<OcrResult>> recognizeImageBytes(Uint8List imageBytes) {
    return _impl.recognizeImageBytes(imageBytes);
  }

  /// Open a native file picker dialog to select an image.
  ///
  /// Returns the selected file path, or `null` if cancelled.
  Future<String?> pickImage() {
    return _impl.pickImage();
  }

  /// Get the platform version string.
  Future<String?> getPlatformVersion() {
    return _impl.getPlatformVersion();
  }

  /// Release native resources.
  ///
  /// Call this when the OCR engine is no longer needed.
  /// After disposal, the engine must be re-initialized before use.
  void dispose() {
    if (_platform is FfiPaddleOcr) {
      (_platform as FfiPaddleOcr).dispose();
    }
    _platform = null;
  }
}
