/// Flutter plugin for offline OCR using PP-OCRv6 with ONNX Runtime C++.
///
/// This plugin provides text detection and recognition capabilities
/// using PaddleOCR PP-OCRv6 models, running entirely offline on Windows.
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
///   detModelPath: r'C:\models\PP-OCRv6_small_det.onnx',
///   recModelPath: r'C:\models\PP-OCRv6_small_rec.onnx',
///   dictPath: r'C:\models\ppocr_keys.txt',
/// );
///
/// // Recognize text from image
/// final results = await ocr.recognizeImage(r'C:\images\test.png');
/// for (final r in results) {
///   print('${r.text} (${r.confidence})');
/// }
/// ```
library;

export 'src/ocr_result.dart';
export 'paddle_ocr_platform_interface.dart' show PaddleOcrPlatform;

import 'dart:typed_data';

import 'paddle_ocr_platform_interface.dart';
import 'src/ocr_result.dart';

/// Main entry point for the PaddleOCR plugin.
///
/// Provides offline text detection and recognition using PP-OCRv6 models
/// powered by ONNX Runtime C++ inference engine.
class PaddleOcr {
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
    return PaddleOcrPlatform.instance.initialize(
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
    return PaddleOcrPlatform.instance.recognizeImage(imagePath);
  }

  /// Recognize text from image byte data.
  ///
  /// [imageBytes]: Raw bytes of the image file.
  ///
  /// Returns a list of [OcrResult], one per detected text region.
  Future<List<OcrResult>> recognizeImageBytes(Uint8List imageBytes) {
    return PaddleOcrPlatform.instance.recognizeImageBytes(imageBytes);
  }

  /// Open a native file picker dialog to select an image.
  ///
  /// Returns the selected file path, or `null` if cancelled.
  Future<String?> pickImage() {
    return PaddleOcrPlatform.instance.pickImage();
  }

  /// Get the platform version string.
  Future<String?> getPlatformVersion() {
    return PaddleOcrPlatform.instance.getPlatformVersion();
  }
}
