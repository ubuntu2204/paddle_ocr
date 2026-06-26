// ignore_for_file: non_constant_identifier_names

import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

// ---------------------------------------------------------------------------
// FFI type definitions
// ---------------------------------------------------------------------------

/// Opaque handle to an OCR engine instance.
typedef PpOcrEngineC = Pointer<Void>;

/// A single OCR result item (C struct).
final class PpOcrResultItemC extends Struct {
  /// 4 points: [x0,y0, x1,y1, x2,y2, x3,y3]
  @Array(8)
  external Array<Double> box;

  external Pointer<Utf8> text;

  @Double()
  external double confidence;
}

/// Array of OCR results (C struct).
final class PpOcrResultArrayC extends Struct {
  external Pointer<PpOcrResultItemC> items;

  @Int32()
  external int count;

  external Pointer<Utf8> error;
}

// ---------------------------------------------------------------------------
// C function signatures (native)
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
// Dart function signatures
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
// FFI bindings
// ---------------------------------------------------------------------------

/// Low-level FFI bindings to the pp_ocr native library.
class PpOcrFfiBindings {
  PpOcrFfiBindings._(this._dylib);

  static PpOcrFfiBindings? _instance;

  /// Get the singleton instance, loading the DLL on first access.
  static PpOcrFfiBindings get instance {
    _instance ??= PpOcrFfiBindings._(_loadLibrary());
    return _instance!;
  }

  final DynamicLibrary _dylib;

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

  /// Create a new OCR engine.
  Pointer<Void> create() => _create();

  /// Destroy an OCR engine.
  void destroy(Pointer<Void> engine) => _destroy(engine);

  /// Initialize the engine with model paths.
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

  /// Check if engine is initialized.
  bool isInitialized(Pointer<Void> engine) =>
      _isInitialized(engine) != 0;

  /// Get last error message.
  String? getLastError(Pointer<Void> engine) {
    final ptr = _getLastError(engine);
    if (ptr == nullptr) return null;
    return ptr.toDartString();
  }

  /// Get version string.
  String version() {
    final ptr = _version();
    return ptr.toDartString();
  }

  /// Recognize text from image file path.
  PpOcrResultArrayC recognizeFile(Pointer<Void> engine, String imagePath) {
    final pathPtr = imagePath.toNativeUtf8();
    try {
      return _recognizeFile(engine, pathPtr);
    } finally {
      calloc.free(pathPtr);
    }
  }

  /// Recognize text from image bytes.
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
