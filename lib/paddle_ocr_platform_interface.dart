import 'dart:typed_data';

import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'pp_ocr_ffi.dart';
import 'src/ocr_result.dart';

/// Platform interface for the pp_ocr plugin.
///
/// Default implementation uses FFI (dart:ffi) for direct native calls.
/// If FFI is unavailable (e.g. DLL not found), it automatically falls back
/// to MethodChannel.
abstract class PaddleOcrPlatform extends PlatformInterface {
  PaddleOcrPlatform() : super(token: _token);

  static final Object _token = Object();

  // Default: try FFI first, which falls back to MethodChannel internally.
  static PaddleOcrPlatform _instance = FfiPaddleOcr();

  static PaddleOcrPlatform get instance => _instance;

  static set instance(PaddleOcrPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  /// Initialize the OCR engine with model paths.
  Future<bool> initialize({
    required String detModelPath,
    required String recModelPath,
    required String dictPath,
  });

  /// Recognize text from an image file path.
  Future<List<OcrResult>> recognizeImage(String imagePath);

  /// Recognize text from image bytes.
  Future<List<OcrResult>> recognizeImageBytes(Uint8List imageBytes);

  /// Open a file picker dialog to select an image. Returns the file path.
  Future<String?> pickImage();

  /// Get platform version string.
  Future<String?> getPlatformVersion();
}
