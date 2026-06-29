// ignore_for_file: non_constant_identifier_names

import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

// ---------------------------------------------------------------------------
// FFI 类型定义
// ---------------------------------------------------------------------------

/// OCR 引擎实例的不透明句柄。
typedef PpOcrEngineC = Pointer<Void>;

/// 单个 OCR 结果项（C 结构体）。
final class PpOcrResultItemC extends Struct {
  /// 4 个角点坐标：[x0,y0, x1,y1, x2,y2, x3,y3]
  @Array(8)
  external Array<Double> box;

  /// 识别出的文本内容（UTF-8 字符串指针）。
  external Pointer<Utf8> text;

  /// 识别置信度。
  @Double()
  external double confidence;
}

/// OCR 结果数组（C 结构体）。
final class PpOcrResultArrayC extends Struct {
  /// 结果项数组指针。
  external Pointer<PpOcrResultItemC> items;

  /// 结果项数量。
  @Int32()
  external int count;

  /// 错误信息指针（为 nullptr 表示无错误）。
  external Pointer<Utf8> error;
}

// ---------------------------------------------------------------------------
// C 函数签名（原生侧）
// ---------------------------------------------------------------------------

typedef _CreateNative = Pointer<Void> Function();
typedef _DestroyNative = Void Function(Pointer<Void>);
typedef _InitializeNative = Int32 Function(
    Pointer<Void>, Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);
typedef _IsInitializedNative = Int32 Function(Pointer<Void>);
typedef _GetLastErrorNative = Pointer<Utf8> Function(Pointer<Void>);
typedef _RecognizeFileNative = PpOcrResultArrayC Function(
    Pointer<Void>, Pointer<Utf8>);
typedef _RecognizeBytesNative = PpOcrResultArrayC Function(
    Pointer<Void>, Pointer<Uint8>, Int32);
typedef _VersionNative = Pointer<Utf8> Function();

// ---------------------------------------------------------------------------
// Dart 函数签名
// ---------------------------------------------------------------------------

typedef _CreateDart = Pointer<Void> Function();
typedef _DestroyDart = void Function(Pointer<Void>);
typedef _InitializeDart = int Function(
    Pointer<Void>, Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);
typedef _IsInitializedDart = int Function(Pointer<Void>);
typedef _GetLastErrorDart = Pointer<Utf8> Function(Pointer<Void>);
typedef _RecognizeFileDart = PpOcrResultArrayC Function(
    Pointer<Void>, Pointer<Utf8>);
typedef _RecognizeBytesDart = PpOcrResultArrayC Function(
    Pointer<Void>, Pointer<Uint8>, int);
typedef _VersionDart = Pointer<Utf8> Function();

// ---------------------------------------------------------------------------
// FFI 绑定
// ---------------------------------------------------------------------------

/// pp_ocr 原生库的底层 FFI 绑定。
class PpOcrFfiBindings {
  PpOcrFfiBindings._(this._dylib);

  /// 单例实例。
  static PpOcrFfiBindings? _instance;

  /// 获取单例实例，首次访问时加载 DLL。
  static PpOcrFfiBindings get instance {
    _instance ??= PpOcrFfiBindings._(_loadLibrary());
    return _instance!;
  }

  /// 已加载的动态库。
  final DynamicLibrary _dylib;

  /// 加载原生动态库。
  ///
  /// 目前仅支持 Windows 平台。
  static DynamicLibrary _loadLibrary() {
    if (Platform.isWindows) {
      return DynamicLibrary.open('pp_ocr_plugin.dll');
    }
    throw UnsupportedError('pp_ocr FFI only supports Windows');
  }

  late final _create =
      _dylib.lookupFunction<_CreateNative, _CreateDart>('pp_ocr_create');
  late final _destroy =
      _dylib.lookupFunction<_DestroyNative, _DestroyDart>('pp_ocr_destroy');
  late final _initialize =
      _dylib.lookupFunction<_InitializeNative, _InitializeDart>(
          'pp_ocr_initialize');
  late final _isInitialized =
      _dylib.lookupFunction<_IsInitializedNative, _IsInitializedDart>(
          'pp_ocr_is_initialized');
  late final _getLastError =
      _dylib.lookupFunction<_GetLastErrorNative, _GetLastErrorDart>(
          'pp_ocr_get_last_error');
  late final _recognizeFile =
      _dylib.lookupFunction<_RecognizeFileNative, _RecognizeFileDart>(
          'pp_ocr_recognize_file');
  late final _recognizeBytes =
      _dylib.lookupFunction<_RecognizeBytesNative, _RecognizeBytesDart>(
          'pp_ocr_recognize_bytes');
  late final _version =
      _dylib.lookupFunction<_VersionNative, _VersionDart>('pp_ocr_version');

  /// 创建新的 OCR 引擎实例。
  Pointer<Void> create() => _create();

  /// 销毁指定的 OCR 引擎实例。
  void destroy(Pointer<Void> engine) => _destroy(engine);

  /// 使用模型路径初始化引擎。
  bool initialize(Pointer<Void> engine, String detPath, String recPath,
      String dictPath) {
    final detPtr = detPath.toNativeUtf8();
    final recPtr = recPath.toNativeUtf8();
    final dictPtr = dictPath.toNativeUtf8();
    try {
      return _initialize(engine, detPtr, recPtr, dictPtr) != 0;
    } finally {
      calloc.free(detPtr);
      calloc.free(recPtr);
      calloc.free(dictPtr);
    }
  }

  /// 检查引擎是否已初始化。
  bool isInitialized(Pointer<Void> engine) =>
      _isInitialized(engine) != 0;

  /// 获取最近一次的错误信息。
  String? getLastError(Pointer<Void> engine) {
    final ptr = _getLastError(engine);
    if (ptr == nullptr) return null;
    return ptr.toDartString();
  }

  /// 获取版本字符串。
  String version() {
    final ptr = _version();
    return ptr.toDartString();
  }

  /// 从图片文件路径识别文字。
  PpOcrResultArrayC recognizeFile(Pointer<Void> engine, String imagePath) {
    final pathPtr = imagePath.toNativeUtf8();
    try {
      return _recognizeFile(engine, pathPtr);
    } finally {
      calloc.free(pathPtr);
    }
  }

  /// 从图片字节数据识别文字。
  PpOcrResultArrayC recognizeBytes(
      Pointer<Void> engine, Uint8List bytes) {
    final ptr = calloc<Uint8>(bytes.length);
    try {
      ptr.asTypedList(bytes.length).setAll(0, bytes);
      return _recognizeBytes(engine, ptr, bytes.length);
    } finally {
      calloc.free(ptr);
    }
  }
}
