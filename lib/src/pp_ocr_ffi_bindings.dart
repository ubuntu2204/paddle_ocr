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

/// OCR 预处理与多分支识别选项（C 结构体）。
///
/// 字段顺序与类型必须与 `cpp/pp_ocr_ffi.h` 里的 `PpOcrOptions` 严格一致；
/// 新增字段只能追加到末尾，并同步升级原生侧的 struct_size 校验。
final class PpOcrOptionsC extends Struct {
  /// 必须等于 pp_ocr_options_size()。
  @Int32()
  external int structSize;

  // ---- 文本检测 ----
  @Double()
  external double detThresh;
  @Double()
  external double detBoxThresh;
  @Double()
  external double detUnclipRatio;
  @Int32()
  external int detMaxSideLen;
  @Int32()
  external int detMinBoxSize;
  @Int32()
  external int detMinBoxArea;

  // ---- 文本识别 ----
  @Int32()
  external int recImgHeight;
  @Int32()
  external int recMaxImgWidth;
  @Int32()
  external int recEnhanceCrops;
  @Int32()
  external int recTryAllBranches;
  @Double()
  external double recMinConfidence;
  @Int32()
  external int recVariantSelect;

  // ---- 文本条二次预处理 ----
  @Int32()
  external int cropBinarizeMode;
  @Int32()
  external int cropAdaptiveBlockSize;
  @Double()
  external double cropAdaptiveC;
  @Int32()
  external int cropEnhanceMode;
  @Double()
  external double cropClaheClipLimit;
  @Int32()
  external int cropClaheGridSize;
  @Double()
  external double cropSharpenAmount;
  @Int32()
  external int cropMorphCloseKernel;
  @Int32()
  external int cropMorphOpenKernel;
  @Int32()
  external int cropColorMode;

  // ---- 多分支与区域合并 ----
  @Uint32()
  external int branches;
  @Double()
  external double mergeIouThreshold;
  @Double()
  external double mergeContainThreshold;
  @Int32()
  external int stitchSameLine;
  @Double()
  external double lineMinYOverlap;
  @Double()
  external double lineMaxGapRatio;
  @Double()
  external double lineMaxHeightDiffRatio;
  @Double()
  external double lineMaxWidthRatio;
  @Double()
  external double regionPadRatio;

  /// 识别窗沿行方向外扩比例（相对字高）。
  @Double()
  external double regionPadLongRatio;
  @Int32()
  external int maxRegions;
  @Int32()
  external int sortOrder;

  // ---- 感兴趣区域 ----
  @Int32()
  external int roiX;
  @Int32()
  external int roiY;
  @Int32()
  external int roiW;
  @Int32()
  external int roiH;

  // ---- 诊断 ----
  @Int32()
  external int verboseStats;
}

// ---------------------------------------------------------------------------
// C 函数签名（原生侧）
// ---------------------------------------------------------------------------

typedef _CreateNative = Pointer<Void> Function();
typedef _DestroyNative = Void Function(Pointer<Void>);
typedef _InitializeNative =
    Int32 Function(Pointer<Void>, Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);
typedef _IsInitializedNative = Int32 Function(Pointer<Void>);
typedef _GetLastErrorNative = Pointer<Utf8> Function(Pointer<Void>);
typedef _RecognizeFileNative =
    PpOcrResultArrayC Function(Pointer<Void>, Pointer<Utf8>);
typedef _RecognizeBytesNative =
    PpOcrResultArrayC Function(Pointer<Void>, Pointer<Uint8>, Int32);
typedef _VersionNative = Pointer<Utf8> Function();
typedef _OptionsSizeNative = Int32 Function();
typedef _OptionsInitNative = Void Function(Pointer<PpOcrOptionsC>);
typedef _OptionsPresetNative = Int32 Function(Int32, Pointer<PpOcrOptionsC>);
typedef _PresetNameNative = Pointer<Utf8> Function(Int32);
typedef _OptionsSanitizeNative = Int32 Function(Pointer<PpOcrOptionsC>);
typedef _SetDefaultOptionsNative =
    Int32 Function(Pointer<Void>, Pointer<PpOcrOptionsC>);
typedef _ClearDefaultOptionsNative = Void Function(Pointer<Void>);
typedef _RecognizeFileExNative =
    PpOcrResultArrayC Function(
      Pointer<Void>,
      Pointer<Utf8>,
      Pointer<PpOcrOptionsC>,
    );
typedef _RecognizeBytesExNative =
    PpOcrResultArrayC Function(
      Pointer<Void>,
      Pointer<Uint8>,
      Int32,
      Pointer<PpOcrOptionsC>,
    );

// ---------------------------------------------------------------------------
// Dart 函数签名
// ---------------------------------------------------------------------------

typedef _CreateDart = Pointer<Void> Function();
typedef _DestroyDart = void Function(Pointer<Void>);
typedef _InitializeDart =
    int Function(Pointer<Void>, Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>);
typedef _IsInitializedDart = int Function(Pointer<Void>);
typedef _GetLastErrorDart = Pointer<Utf8> Function(Pointer<Void>);
typedef _RecognizeFileDart =
    PpOcrResultArrayC Function(Pointer<Void>, Pointer<Utf8>);
typedef _RecognizeBytesDart =
    PpOcrResultArrayC Function(Pointer<Void>, Pointer<Uint8>, int);
typedef _VersionDart = Pointer<Utf8> Function();
typedef _OptionsSizeDart = int Function();
typedef _OptionsInitDart = void Function(Pointer<PpOcrOptionsC>);
typedef _OptionsPresetDart = int Function(int, Pointer<PpOcrOptionsC>);
typedef _PresetNameDart = Pointer<Utf8> Function(int);
typedef _OptionsSanitizeDart = int Function(Pointer<PpOcrOptionsC>);
typedef _SetDefaultOptionsDart =
    int Function(Pointer<Void>, Pointer<PpOcrOptionsC>);
typedef _ClearDefaultOptionsDart = void Function(Pointer<Void>);
typedef _RecognizeFileExDart =
    PpOcrResultArrayC Function(
      Pointer<Void>,
      Pointer<Utf8>,
      Pointer<PpOcrOptionsC>,
    );
typedef _RecognizeBytesExDart =
    PpOcrResultArrayC Function(
      Pointer<Void>,
      Pointer<Uint8>,
      int,
      Pointer<PpOcrOptionsC>,
    );

// ---------------------------------------------------------------------------
// 原子操作（宿主编排自定义识别管线）的 C 签名
// ---------------------------------------------------------------------------

/// 预处理变换参数（对应 PpOcrTransformParams）。
final class PpOcrTransformParamsC extends Struct {
  @Int32()
  external int structSize;

  @Int32()
  external int scalePercent;
  @Int32()
  external int binarizeMode;
  @Int32()
  external int adaptiveBlockSize;
  @Double()
  external double adaptiveC;
  @Int32()
  external int enhanceMode;
  @Double()
  external double claheClipLimit;
  @Int32()
  external int claheGridSize;
  @Double()
  external double sharpenAmount;
  @Int32()
  external int morphOpenKernel;
  @Int32()
  external int morphCloseKernel;
  @Int32()
  external int colorMode;
}

/// 引擎持有的 PNG 图像缓冲区。
final class PpOcrImageBufferC extends Struct {
  external Pointer<Uint8> data;

  @Int32()
  external int size;
}

/// 检测框（4 角点 + 来源）。
final class PpOcrBoxInputC extends Struct {
  @Array(8)
  external Array<Double> box;

  @Int32()
  external int branchIndex;

  @Double()
  external double score;
}

/// 检测框数组（引擎持有）。
final class PpOcrBoxArrayC extends Struct {
  external Pointer<PpOcrBoxInputC> items;

  @Int32()
  external int count;

  @Int32()
  external int imageWidth;

  @Int32()
  external int imageHeight;

  external Pointer<Utf8> error;
}

/// 单窗识别结果（引擎持有）。
final class PpOcrTextResultC extends Struct {
  external Pointer<Utf8> text;

  @Double()
  external double confidence;
}

/// 多窗识别结果数组（引擎持有，下一次调用前有效）。
final class PpOcrTextResultArrayC extends Struct {
  external Pointer<PpOcrTextResultC> items;

  @Int32()
  external int count;

  external Pointer<Utf8> error;
}

typedef _TransformParamsSizeNative = Int32 Function();
typedef _TransformParamsInitNative = Void Function(
  Pointer<PpOcrTransformParamsC>,
);
typedef _PreprocessBytesNative =
    Int32 Function(Pointer<Void>, Pointer<Uint8>, Int32,
        Pointer<PpOcrTransformParamsC>, Pointer<PpOcrImageBufferC>);
typedef _DetectBytesNative =
    Int32 Function(Pointer<Void>, Pointer<Uint8>, Int32,
        Pointer<PpOcrOptionsC>, Pointer<PpOcrBoxArrayC>);
typedef _MergeBoxesNative =
    Int32 Function(Pointer<Void>, Pointer<PpOcrBoxInputC>, Int32,
        Pointer<PpOcrOptionsC>, Int32, Int32, Pointer<PpOcrBoxArrayC>);
typedef _RecognizeBoxBytesNative =
    Int32 Function(Pointer<Void>, Pointer<Uint8>, Int32, Pointer<Double>,
        Pointer<PpOcrOptionsC>, Pointer<PpOcrTextResultC>);
typedef _RecognizeBoxesBytesNative =
    Int32 Function(Pointer<Void>, Pointer<Uint8>, Int32, Pointer<Double>, Int32,
        Pointer<PpOcrOptionsC>, Pointer<PpOcrTextResultArrayC>);

typedef _TransformParamsSizeDart = int Function();
typedef _TransformParamsInitDart = void Function(
  Pointer<PpOcrTransformParamsC>,
);
typedef _PreprocessBytesDart =
    int Function(Pointer<Void>, Pointer<Uint8>, int,
        Pointer<PpOcrTransformParamsC>, Pointer<PpOcrImageBufferC>);
typedef _DetectBytesDart =
    int Function(Pointer<Void>, Pointer<Uint8>, int,
        Pointer<PpOcrOptionsC>, Pointer<PpOcrBoxArrayC>);
typedef _MergeBoxesDart =
    int Function(Pointer<Void>, Pointer<PpOcrBoxInputC>, int,
        Pointer<PpOcrOptionsC>, int, int, Pointer<PpOcrBoxArrayC>);
typedef _RecognizeBoxBytesDart =
    int Function(Pointer<Void>, Pointer<Uint8>, int, Pointer<Double>,
        Pointer<PpOcrOptionsC>, Pointer<PpOcrTextResultC>);
typedef _RecognizeBoxesBytesDart =
    int Function(Pointer<Void>, Pointer<Uint8>, int, Pointer<Double>, int,
        Pointer<PpOcrOptionsC>, Pointer<PpOcrTextResultArrayC>);

// ---------------------------------------------------------------------------
// FFI 绑定
// ---------------------------------------------------------------------------

/// pp_ocr 原生库的底层 FFI 绑定。
///
/// 仅支持 Windows 与 Linux；其他平台加载即抛 UnsupportedError。
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
  /// 支持 Windows（pp_ocr_plugin.dll）和 Linux（libpp_ocr_plugin.so）。
  /// macOS/Android/iOS 目前尚未实现。
  static DynamicLibrary _loadLibrary() {
    if (Platform.isWindows) {
      return DynamicLibrary.open('pp_ocr_plugin.dll');
    }
    if (Platform.isLinux) {
      // Flutter Linux 插件产物名由 linux/CMakeLists.txt 的 PLUGIN_NAME
      // 决定（pp_ocr_plugin），默认共享库文件名为 libpp_ocr_plugin.so。
      // 应用发布后与可执行文件同层交付，通过 $ORIGIN RPATH 定位，
      // 因此直接以文件名 open 即可。
      return DynamicLibrary.open('libpp_ocr_plugin.so');
    }
    throw UnsupportedError('pp_ocr FFI only supports Windows and Linux');
  }

  late final _create = _dylib.lookupFunction<_CreateNative, _CreateDart>(
    'pp_ocr_create',
  );
  late final _destroy = _dylib.lookupFunction<_DestroyNative, _DestroyDart>(
    'pp_ocr_destroy',
  );
  late final _initialize = _dylib
      .lookupFunction<_InitializeNative, _InitializeDart>('pp_ocr_initialize');
  late final _isInitialized = _dylib
      .lookupFunction<_IsInitializedNative, _IsInitializedDart>(
        'pp_ocr_is_initialized',
      );
  late final _getLastError = _dylib
      .lookupFunction<_GetLastErrorNative, _GetLastErrorDart>(
        'pp_ocr_get_last_error',
      );
  late final _recognizeFile = _dylib
      .lookupFunction<_RecognizeFileNative, _RecognizeFileDart>(
        'pp_ocr_recognize_file',
      );
  late final _recognizeBytes = _dylib
      .lookupFunction<_RecognizeBytesNative, _RecognizeBytesDart>(
        'pp_ocr_recognize_bytes',
      );
  late final _version = _dylib.lookupFunction<_VersionNative, _VersionDart>(
    'pp_ocr_version',
  );
  late final _optionsSize = _dylib
      .lookupFunction<_OptionsSizeNative, _OptionsSizeDart>(
        'pp_ocr_options_size',
      );
  late final _optionsInit = _dylib
      .lookupFunction<_OptionsInitNative, _OptionsInitDart>(
        'pp_ocr_options_init',
      );
  late final _optionsPreset = _dylib
      .lookupFunction<_OptionsPresetNative, _OptionsPresetDart>(
        'pp_ocr_options_preset',
      );
  late final _presetName = _dylib
      .lookupFunction<_PresetNameNative, _PresetNameDart>('pp_ocr_preset_name');
  late final _optionsSanitize = _dylib
      .lookupFunction<_OptionsSanitizeNative, _OptionsSanitizeDart>(
        'pp_ocr_options_sanitize',
      );
  late final _setDefaultOptions = _dylib
      .lookupFunction<_SetDefaultOptionsNative, _SetDefaultOptionsDart>(
        'pp_ocr_set_default_options',
      );
  late final _clearDefaultOptions = _dylib
      .lookupFunction<_ClearDefaultOptionsNative, _ClearDefaultOptionsDart>(
        'pp_ocr_clear_default_options',
      );
  late final _recognizeFileEx = _dylib
      .lookupFunction<_RecognizeFileExNative, _RecognizeFileExDart>(
        'pp_ocr_recognize_file_ex',
      );
  late final _recognizeBytesEx = _dylib
      .lookupFunction<_RecognizeBytesExNative, _RecognizeBytesExDart>(
        'pp_ocr_recognize_bytes_ex',
      );
  late final _transformParamsSize = _dylib
      .lookupFunction<_TransformParamsSizeNative, _TransformParamsSizeDart>(
        'pp_ocr_transform_params_size',
      );
  late final _transformParamsInit = _dylib
      .lookupFunction<_TransformParamsInitNative, _TransformParamsInitDart>(
        'pp_ocr_transform_params_init',
      );
  late final _preprocessBytes = _dylib
      .lookupFunction<_PreprocessBytesNative, _PreprocessBytesDart>(
        'pp_ocr_preprocess_bytes',
      );
  late final _detectBytes = _dylib
      .lookupFunction<_DetectBytesNative, _DetectBytesDart>(
        'pp_ocr_detect_bytes',
      );
  late final _mergeBoxes = _dylib
      .lookupFunction<_MergeBoxesNative, _MergeBoxesDart>('pp_ocr_merge_boxes');
  late final _recognizeBoxBytes = _dylib
      .lookupFunction<_RecognizeBoxBytesNative, _RecognizeBoxBytesDart>(
        'pp_ocr_recognize_box_bytes',
      );
  late final _recognizeBoxesBytes = _dylib
      .lookupFunction<_RecognizeBoxesBytesNative, _RecognizeBoxesBytesDart>(
        'pp_ocr_recognize_boxes_bytes',
      );

  // -------------------------------------------------------------------------
  // 原子操作（宿主编排自定义识别管线）
  // -------------------------------------------------------------------------

  /// 原生侧 PpOcrTransformParams 的字节大小。
  int transformParamsSize() => _transformParamsSize();

  /// 把指针指向的结构体填成默认变换参数。
  void transformParamsInit(Pointer<PpOcrTransformParamsC> out) =>
      _transformParamsInit(out);

  /// 原子 1：整图预处理变换，输出 PNG 字节（引擎持有）。
  ({Pointer<Uint8> data, int size})? preprocessBytes(
    Pointer<Void> engine,
    Uint8List bytes,
    Pointer<PpOcrTransformParamsC> params,
  ) {
    final ptr = calloc<Uint8>(bytes.length);
    final out = calloc<PpOcrImageBufferC>();
    try {
      ptr.asTypedList(bytes.length).setAll(0, bytes);
      if (_preprocessBytes(engine, ptr, bytes.length, params, out) == 0) {
        return null;
      }
      return (data: out.ref.data, size: out.ref.size);
    } finally {
      calloc.free(ptr);
      calloc.free(out);
    }
  }

  /// 原子 2：文本检测。失败返回 null（用 getLastError 取原因）。
  ///
  /// 返回的是「字段快照」而不是 FFI 结构体值：
  /// [Pointer<Struct>.ref] 在 dart:ffi 里的语义是**视图**（SDK 文档原话
  /// "A Dart view of the struct referenced by this pointer... backed by
  /// native memory at address"），并不是拷贝。若 `return out.ref` 之后再把
  /// `out` free 掉，调用方读到的就是已被 malloc 复用的野内存 —— 字段会随
  /// 机变成垃圾值，进而在 Dart 侧解引用 `items` 时 SIGSEGV（崩溃 PC 落在
  /// JIT code 区，极易误诊为「Dart print/libc write」）。
  /// 因此所有出参结构体都必须在 free 之前把字段读成 Dart 值 / 引擎持有的
  /// 指针（引擎缓冲区按 ABI 约定「下一次调用前有效」）。
  ///
  /// [items] 指向引擎持有的框数组，在下一次调用本引擎任何接口前有效。
  ({Pointer<PpOcrBoxInputC> items, int count, int imageWidth, int imageHeight})?
      detectBytes(
    Pointer<Void> engine,
    Uint8List bytes,
    Pointer<PpOcrOptionsC> options,
  ) {
    final ptr = calloc<Uint8>(bytes.length);
    final out = calloc<PpOcrBoxArrayC>();
    try {
      ptr.asTypedList(bytes.length).setAll(0, bytes);
      if (_detectBytes(engine, ptr, bytes.length, options, out) == 0) {
        return null;
      }
      // free 之前取快照（见上方关于 .ref 视图语义的说明）。
      return (
        items: out.ref.items,
        count: out.ref.count,
        imageWidth: out.ref.imageWidth,
        imageHeight: out.ref.imageHeight,
      );
    } finally {
      calloc.free(ptr);
      calloc.free(out);
    }
  }

  /// 原子 3：合并多路检测框为识别窗。失败返回 null。
  ///
  /// 同样返回字段快照而非结构体视图，原因见 [detectBytes] 的文档。
  ({Pointer<PpOcrBoxInputC> items, int count, int imageWidth, int imageHeight})?
      mergeBoxes(
    Pointer<Void> engine,
    Pointer<PpOcrBoxInputC> boxes,
    int count,
    Pointer<PpOcrOptionsC> options,
    int imageWidth,
    int imageHeight,
  ) {
    final out = calloc<PpOcrBoxArrayC>();
    try {
      if (_mergeBoxes(engine, boxes, count, options, imageWidth, imageHeight,
              out) ==
          0) {
        return null;
      }
      return (
        items: out.ref.items,
        count: out.ref.count,
        imageWidth: out.ref.imageWidth,
        imageHeight: out.ref.imageHeight,
      );
    } finally {
      calloc.free(out);
    }
  }

  /// 原子 4：识别一个文本窗。失败返回 null。
  ({String text, double confidence})? recognizeBoxBytes(
    Pointer<Void> engine,
    Uint8List bytes,
    List<double> box8,
    Pointer<PpOcrOptionsC> options,
  ) {
    assert(box8.length == 8);
    final ptr = calloc<Uint8>(bytes.length);
    final boxPtr = calloc<Double>(8);
    final out = calloc<PpOcrTextResultC>();
    try {
      ptr.asTypedList(bytes.length).setAll(0, bytes);
      boxPtr.asTypedList(8).setAll(0, box8);
      if (_recognizeBoxBytes(engine, ptr, bytes.length, boxPtr, options, out) ==
          0) {
        return null;
      }
      final text = out.ref.text == nullptr ? '' : out.ref.text.toDartString();
      return (text: text, confidence: out.ref.confidence);
    } finally {
      calloc.free(ptr);
      calloc.free(boxPtr);
      calloc.free(out);
    }
  }

  /// 原子 4（批量）：在**同一张图**上识别多个文本窗。
  ///
  /// [boxes] 为 count × 8 个 double（[x0,y0, x1,y1, x2,y2, x3,y3]），坐标在
  /// 输入图坐标系。返回与入参一一对应的结果列表（识别失败的项为空串）；
  /// 失败返回 null。相比逐个窗调用 [recognizeBoxBytes]：一次 FFI 穿越、
  /// 图像只解码一次、原生侧线程池并行推理。
  List<({String text, double confidence})>? recognizeBoxesBytes(
    Pointer<Void> engine,
    Uint8List bytes,
    Pointer<Double> boxes,
    int count,
    Pointer<PpOcrOptionsC> options,
  ) {
    final ptr = calloc<Uint8>(bytes.length);
    final out = calloc<PpOcrTextResultArrayC>();
    try {
      ptr.asTypedList(bytes.length).setAll(0, bytes);
      if (_recognizeBoxesBytes(
            engine,
            ptr,
            bytes.length,
            boxes,
            count,
            options,
            out,
          ) ==
          0) {
        return null;
      }
      final items = out.ref.items;
      if (items == nullptr) return const [];
      return [
        for (int i = 0; i < out.ref.count; i++)
          (
            text: items[i].text == nullptr ? '' : items[i].text.toDartString(),
            confidence: items[i].confidence,
          ),
      ];
    } finally {
      calloc.free(ptr);
      calloc.free(out);
    }
  }

  /// 创建新的 OCR 引擎实例。
  Pointer<Void> create() => _create();

  /// 销毁指定的 OCR 引擎实例。
  void destroy(Pointer<Void> engine) => _destroy(engine);

  /// 使用模型路径初始化引擎。
  bool initialize(
    Pointer<Void> engine,
    String detPath,
    String recPath,
    String dictPath,
  ) {
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
  bool isInitialized(Pointer<Void> engine) => _isInitialized(engine) != 0;

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
  PpOcrResultArrayC recognizeBytes(Pointer<Void> engine, Uint8List bytes) {
    final ptr = calloc<Uint8>(bytes.length);
    try {
      ptr.asTypedList(bytes.length).setAll(0, bytes);
      return _recognizeBytes(engine, ptr, bytes.length);
    } finally {
      calloc.free(ptr);
    }
  }

  // -------------------------------------------------------------------------
  // 预处理与多分支选项
  // -------------------------------------------------------------------------

  /// 原生侧 PpOcrOptions 的字节大小，用于核对结构体版本。
  int optionsSize() => _optionsSize();

  /// 把指针指向的结构体填成「标准」默认值。
  void optionsInit(Pointer<PpOcrOptionsC> out) => _optionsInit(out);

  /// 把指针指向的结构体填成某个预设的取值。返回 1 表示成功。
  bool optionsPreset(int presetId, Pointer<PpOcrOptionsC> out) =>
      _optionsPreset(presetId, out) != 0;

  /// 预设的可读名称。
  String presetName(int presetId) {
    final ptr = _presetName(presetId);
    if (ptr == nullptr) return '';
    return ptr.toDartString();
  }

  /// 就地修正非法参数，返回被修正的字段个数（0 = 全部合法，-1 = 布局不匹配）。
  int optionsSanitize(Pointer<PpOcrOptionsC> options) =>
      _optionsSanitize(options);

  /// 设置引擎级默认选项。[options] 为 null 表示不修改。
  bool setDefaultOptions(
    Pointer<Void> engine,
    Pointer<PpOcrOptionsC>? options,
  ) => _setDefaultOptions(engine, options ?? nullptr) != 0;

  /// 清除引擎级默认选项，回到标准单分支行为。
  void clearDefaultOptions(Pointer<Void> engine) =>
      _clearDefaultOptions(engine);

  /// 带选项从图片文件识别。[options] 为 null 时走引擎级默认。
  PpOcrResultArrayC recognizeFileEx(
    Pointer<Void> engine,
    String imagePath,
    Pointer<PpOcrOptionsC> options,
  ) {
    final pathPtr = imagePath.toNativeUtf8();
    try {
      return _recognizeFileEx(engine, pathPtr, options);
    } finally {
      calloc.free(pathPtr);
    }
  }

  /// 带选项从图片字节识别。
  PpOcrResultArrayC recognizeBytesEx(
    Pointer<Void> engine,
    Uint8List bytes,
    Pointer<PpOcrOptionsC> options,
  ) {
    final ptr = calloc<Uint8>(bytes.length);
    try {
      ptr.asTypedList(bytes.length).setAll(0, bytes);
      return _recognizeBytesEx(engine, ptr, bytes.length, options);
    } finally {
      calloc.free(ptr);
    }
  }
}
