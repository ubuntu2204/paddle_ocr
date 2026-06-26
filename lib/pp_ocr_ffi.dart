import 'dart:ffi';
import 'dart:typed_data';
import 'dart:ui';

import 'package:ffi/ffi.dart';

import 'paddle_ocr_method_channel.dart';
import 'paddle_ocr_platform_interface.dart';
import 'src/ocr_result.dart';
import 'src/pp_ocr_ffi_bindings.dart';

/// FFI-based implementation of [PaddleOcrPlatform].
///
/// This is the default implementation on Windows. It communicates directly
/// with the native C++ library via dart:ffi, avoiding the overhead of
/// MethodChannel. If FFI is unavailable (e.g. DLL not found), it falls
/// back to [MethodChannelPaddleOcr].
class FfiPaddleOcr extends PaddleOcrPlatform {
  FfiPaddleOcr();

  Pointer<Void>? _engine;
  bool _ffiAvailable = false;
  PaddleOcrPlatform? _fallback;

  /// Try to use FFI; if the DLL cannot be loaded, fall back to MethodChannel.
  PaddleOcrPlatform _getImpl() {
    if (_ffiAvailable && _engine != null && _engine != nullptr) {
      return this;
    }
    _fallback ??= MethodChannelPaddleOcr();
    return _fallback!;
  }

  void _ensureEngine() {
    if (_engine != null) return;
    try {
      final bindings = PpOcrFfiBindings.instance;
      _engine = bindings.create();
      _ffiAvailable = true;
    } catch (e) {
      _ffiAvailable = false;
      _engine = nullptr;
    }
  }

  @override
  Future<bool> initialize({
    required String detModelPath,
    required String recModelPath,
    required String dictPath,
  }) async {
    _ensureEngine();
    if (!_ffiAvailable || _engine == null || _engine == nullptr) {
      return _getImpl().initialize(
        detModelPath: detModelPath,
        recModelPath: recModelPath,
        dictPath: dictPath,
      );
    }

    final engine = _engine!;
    final bindings = PpOcrFfiBindings.instance;
    final ok = bindings.initialize(engine, detModelPath, recModelPath, dictPath);
    if (!ok) {
      final err = bindings.getLastError(engine);
      // ignore: avoid_print
      print('[pp_ocr] FFI initialize failed: $err');
    }
    return ok;
  }

  @override
  Future<List<OcrResult>> recognizeImage(String imagePath) async {
    if (!_ffiAvailable) {
      return _getImpl().recognizeImage(imagePath);
    }
    _ensureEngine();
    if (_engine == null || _engine == nullptr) {
      return _getImpl().recognizeImage(imagePath);
    }

    final engine = _engine!;
    final bindings = PpOcrFfiBindings.instance;
    final resultC = bindings.recognizeFile(engine, imagePath);

    if (resultC.error != nullptr) {
      final err = resultC.error.toDartString();
      throw Exception('OCR recognition failed: $err');
    }

    return _parseResultsC(resultC);
  }

  @override
  Future<List<OcrResult>> recognizeImageBytes(Uint8List imageBytes) async {
    if (!_ffiAvailable) {
      return _getImpl().recognizeImageBytes(imageBytes);
    }
    _ensureEngine();
    if (_engine == null || _engine == nullptr) {
      return _getImpl().recognizeImageBytes(imageBytes);
    }

    final engine = _engine!;
    final bindings = PpOcrFfiBindings.instance;
    final resultC = bindings.recognizeBytes(engine, imageBytes);

    if (resultC.error != nullptr) {
      final err = resultC.error.toDartString();
      throw Exception('OCR recognition failed: $err');
    }

    return _parseResultsC(resultC);
  }

  @override
  Future<String?> pickImage() async {
    // pickImage uses native file dialog — delegate to MethodChannel
    return _getImpl().pickImage();
  }

  @override
  Future<String?> getPlatformVersion() async {
    if (_ffiAvailable) {
      try {
        return PpOcrFfiBindings.instance.version();
      } catch (_) {
        // fall through to method channel
      }
    }
    return _getImpl().getPlatformVersion();
  }

  /// Parse C result array into Dart [OcrResult] list.
  List<OcrResult> _parseResultsC(PpOcrResultArrayC resultC) {
    final results = <OcrResult>[];
    final count = resultC.count;
    if (count <= 0 || resultC.items == nullptr) return results;

    final items = resultC.items;
    for (int i = 0; i < count; i++) {
      final item = items[i];
      final box = <Offset>[];
      for (int j = 0; j < 4; j++) {
        final x = item.box[j * 2];
        final y = item.box[j * 2 + 1];
        box.add(Offset(x, y));
      }

      String text = '';
      if (item.text != nullptr) {
        text = item.text.toDartString();
      }

      results.add(OcrResult(
        box: box,
        text: text,
        confidence: item.confidence,
      ));
    }
    return results;
  }

  /// Dispose the engine and free native resources.
  void dispose() {
    if (_engine != null && _engine != nullptr && _ffiAvailable) {
      try {
        PpOcrFfiBindings.instance.destroy(_engine!);
      } catch (_) {}
    }
    _engine = null;
    _ffiAvailable = false;
  }
}
