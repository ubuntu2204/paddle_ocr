import 'dart:ffi';
import 'dart:typed_data';
import 'dart:ui';

import 'package:ffi/ffi.dart';

import 'paddle_ocr_plugin_channel.dart';
import 'paddle_ocr_platform_interface.dart';
import 'src/ocr_options.dart';
import 'src/ocr_pipeline.dart';
import 'src/ocr_preprocessor.dart';
import 'src/ocr_result.dart';
import 'src/pp_ocr_ffi_bindings.dart';

/// [PaddleOcrPlatform] 的唯一实现：FFI（dart:ffi）直连原生库。
///
/// 识别链路没有任何回退 —— MethodChannel 传输整图字节的序列化开销
/// 不可接受，也不再保留降级桩。原生库加载失败时错误会直接抛给调用方，
/// 便于第一时间暴露部署问题（缺 DLL / so、架构不匹配等）。
class FfiPaddleOcr extends PaddleOcrPlatform {
  FfiPaddleOcr();

  /// 原生 OCR 引擎的不透明指针。
  Pointer<Void>? _engine;

  /// FFI 是否可用的标志位。
  bool _ffiAvailable = false;

  /// 依赖插件注册器的小功能（文件对话框 / 平台信息）。
  PluginChannelPaddleOcr get _pluginChannel =>
      _pluginChannelInstance ??= PluginChannelPaddleOcr();
  PluginChannelPaddleOcr? _pluginChannelInstance;

  /// 确保引擎已创建。原生库不可用时抛 [StateError]。
  void _ensureEngine() {
    if (_engine != null) return;
    try {
      final bindings = PpOcrFfiBindings.instance;
      _engine = bindings.create();
      _ffiAvailable = true;
    } catch (e) {
      _ffiAvailable = false;
      _engine = nullptr;
      throw StateError(
        'pp_ocr native library is not available: $e. '
        '识别链路只支持 FFI，请检查原生库（pp_ocr_plugin.dll / '
        'libpp_ocr_plugin.so）是否随应用正确分发。',
      );
    }
  }

  /// FFI 引擎指针；未初始化时抛 [StateError]。
  Pointer<Void> get _requiredEngine {
    _ensureEngine();
    final engine = _engine;
    if (!_ffiAvailable || engine == null || engine == nullptr) {
      throw StateError('pp_ocr native engine is not available');
    }
    return engine;
  }

  @override
  Future<bool> initialize({
    required String detModelPath,
    required String recModelPath,
    required String dictPath,
  }) async {
    final engine = _requiredEngine;
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
  Future<List<OcrResult>> recognizeImage(
    String imagePath, {
    OcrOptions? options,
  }) async {
    final engine = _requiredEngine;
    final bindings = PpOcrFfiBindings.instance;
    final PpOcrResultArrayC resultC;
    if (options == null) {
      resultC = bindings.recognizeFile(engine, imagePath);
    } else {
      final optsPtr = options.toNativeStruct();
      try {
        resultC = bindings.recognizeFileEx(engine, imagePath, optsPtr);
      } finally {
        calloc.free(optsPtr);
      }
    }

    if (resultC.error != nullptr) {
      final err = resultC.error.toDartString();
      throw Exception('OCR recognition failed: $err');
    }

    return _parseResultsC(resultC);
  }

  @override
  Future<List<OcrResult>> recognizeImageBytes(
    Uint8List imageBytes, {
    OcrOptions? options,
  }) async {
    final engine = _requiredEngine;
    final bindings = PpOcrFfiBindings.instance;
    final PpOcrResultArrayC resultC;
    if (options == null) {
      resultC = bindings.recognizeBytes(engine, imageBytes);
    } else {
      final optsPtr = options.toNativeStruct();
      try {
        resultC = bindings.recognizeBytesEx(engine, imageBytes, optsPtr);
      } finally {
        calloc.free(optsPtr);
      }
    }

    if (resultC.error != nullptr) {
      final err = resultC.error.toDartString();
      throw Exception('OCR recognition failed: $err');
    }

    return _parseResultsC(resultC);
  }

  /// 把选项设为引擎级默认。
  @override
  Future<bool> setDefaultOptions(OcrOptions options) async {
    final engine = _requiredEngine;
    final bindings = PpOcrFfiBindings.instance;
    final optsPtr = options.toNativeStruct();
    try {
      final ok = bindings.setDefaultOptions(engine, optsPtr);
      if (!ok) {
        // ignore: avoid_print
        print(
          '[pp_ocr] setDefaultOptions failed: '
          '${bindings.getLastError(engine)}',
        );
      }
      return ok;
    } finally {
      calloc.free(optsPtr);
    }
  }

  /// 用宿主注入的预处理变体跑编排管线（插件接口）。
  @override
  Future<List<OcrResult>> runPipeline(
    Uint8List imageBytes,
    List<OcrPreprocessor> preprocessors, {
    OcrOptions? options,
  }) async {
    final engine = _requiredEngine;
    final bindings = PpOcrFfiBindings.instance;
    return runOcrPipeline(
      bindings: bindings,
      engine: engine,
      imageBytes: imageBytes,
      preprocessors: preprocessors,
      options: options ?? OcrOptions.preset(OcrPreset.surveillance),
    );
  }

  @override
  void clearDefaultOptions() {
    if (_engine == null || _engine == nullptr || !_ffiAvailable) return;
    try {
      PpOcrFfiBindings.instance.clearDefaultOptions(_engine!);
    } catch (_) {
      // 旧版原生库没有这个符号，忽略即可。
    }
  }

  @override
  Future<String?> pickImage() => _pluginChannel.pickImage();

  @override
  Future<String?> getPlatformVersion() async {
    // 优先读原生库版本（真实版本核对），失败再问平台。
    _ensureEngine();
    if (_ffiAvailable && _engine != null && _engine != nullptr) {
      try {
        return PpOcrFfiBindings.instance.version();
      } catch (_) {
        // 继续走平台查询。
      }
    }
    return _pluginChannel.getPlatformVersion();
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

      results.add(OcrResult(box: box, text: text, confidence: item.confidence));
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
