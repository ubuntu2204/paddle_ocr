import 'dart:ffi';
import 'dart:typed_data';
import 'dart:ui';

import 'package:ffi/ffi.dart';

import 'paddle_ocr_method_channel.dart';
import 'paddle_ocr_platform_interface.dart';
import 'src/ocr_result.dart';
import 'src/pp_ocr_ffi_bindings.dart';

/// [PaddleOcrPlatform] 的 FFI 实现。
///
/// 这是 Windows 平台的默认实现。它通过 dart:ffi 直接与原生 C++ 库通信，
/// 避免了 MethodChannel 的开销。当 FFI 不可用时（例如找不到 DLL），
/// 会自动回退到 [MethodChannelPaddleOcr]。
class FfiPaddleOcr extends PaddleOcrPlatform {
  FfiPaddleOcr();

  /// 原生 OCR 引擎的不透明指针。
  Pointer<Void>? _engine;

  /// FFI 是否可用的标志位。
  bool _ffiAvailable = false;

  /// MethodChannel 回退实现，按需创建。
  MethodChannelPaddleOcr? _fallback;

  /// 尝试使用 FFI；若 DLL 无法加载，则回退到 MethodChannel。
  PaddleOcrPlatform _getImpl() {
    if (_ffiAvailable && _engine != null && _engine != nullptr) {
      return this;
    }
    _fallback ??= MethodChannelPaddleOcr();
    return _fallback!;
  }

  /// 确保引擎已创建，若创建失败则标记 FFI 不可用。
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

  /// 对于需要 Flutter 插件注册器的功能（例如原生文件对话框），
  /// 始终使用 MethodChannel。FFI 无法弹出文件选择框，因此不能
  /// 通过 [_getImpl] 路由（当 FFI 激活时它会返回 [this]，
  /// 会导致无限递归）。
  MethodChannelPaddleOcr get _methodChannel =>
      _fallback ??= MethodChannelPaddleOcr();

  @override
  Future<String?> pickImage() async {
    // pickImage 使用 Windows 原生文件对话框 —— 仅限 MethodChannel。
    return _methodChannel.pickImage();
  }

  @override
  Future<String?> getPlatformVersion() async {
    if (_ffiAvailable) {
      try {
        return PpOcrFfiBindings.instance.version();
      } catch (_) {
        // 回退到 MethodChannel
      }
    }
    return _methodChannel.getPlatformVersion();
  }

  /// 将 C 结果数组解析为 Dart [OcrResult] 列表。
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

  /// 释放引擎并清理原生资源。
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
