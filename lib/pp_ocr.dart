/// 基于 PP-OCRv6 与 ONNX Runtime 的离线 OCR Flutter 插件。
///
/// 本插件使用 PaddleOCR PP-OCRv6 模型提供文字检测与识别能力，
/// 完全在 Windows 上离线运行。
///
/// 插件支持两种后端：
/// - **FFI**（默认）：通过 `dart:ffi` 直接调用原生库，延迟更低
/// - **MethodChannel**（回退）：当 FFI DLL 不可用时使用
///
/// ## 快速开始
///
/// ```dart
/// import 'package:pp_ocr/pp_ocr.dart';
///
/// final ocr = PaddleOcr();
///
/// // 使用模型路径初始化
/// await ocr.initialize(
///   detModelPath: r'C:\models\det.onnx',
///   recModelPath: r'C:\models\inference.onnx',
///   dictPath: r'C:\models\ppocr_v6_dict.txt',
/// );
///
/// // 从图片识别文字
/// final results = await ocr.recognizeImage(r'C:\images\test.png');
/// for (final r in results) {
///   print('${r.text} (${r.confidence})');
/// }
///
/// // 使用完毕后记得释放资源
/// ocr.dispose();
/// ```
library;

export 'src/ocr_result.dart';
export 'paddle_ocr_platform_interface.dart' show PaddleOcrPlatform;

import 'dart:typed_data';

import 'paddle_ocr_platform_interface.dart';
import 'pp_ocr_ffi.dart';
import 'src/ocr_result.dart';

/// PP-OCR 插件的主入口类。
///
/// 基于 ONNX Runtime C++ 推理引擎提供离线文字检测与识别能力。
///
/// 默认使用 FFI（dart:ffi）直接调用原生库，当 FFI DLL 不可用时
/// 会自动回退到 MethodChannel 实现。
class PaddleOcr {
  /// 平台实现实例，延迟初始化。
  PaddleOcrPlatform? _platform;

  /// 获取平台实现实例，首次访问时创建。
  PaddleOcrPlatform get _impl => _platform ??= PaddleOcrPlatform.instance;

  /// 初始化 OCR 引擎。
  ///
  /// 在调用任何识别方法之前必须先调用此方法。
  ///
  /// [detModelPath]：文字检测 ONNX 模型文件路径。
  /// [recModelPath]：文字识别 ONNX 模型文件路径。
  /// [dictPath]：字符字典文本文件路径。
  ///
  /// 返回 `true` 表示初始化成功。
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

  /// 从图片文件识别文字。
  ///
  /// [imagePath]：图片文件的绝对路径（支持 png、jpg、bmp、tiff）。
  ///
  /// 返回 [OcrResult] 列表，每个检测结果对应一个文字区域。
  Future<List<OcrResult>> recognizeImage(String imagePath) {
    return _impl.recognizeImage(imagePath);
  }

  /// 从图片字节数据识别文字。
  ///
  /// [imageBytes]：图片文件的原始字节数据。
  ///
  /// 返回 [OcrResult] 列表，每个检测结果对应一个文字区域。
  Future<List<OcrResult>> recognizeImageBytes(Uint8List imageBytes) {
    return _impl.recognizeImageBytes(imageBytes);
  }

  /// 打开原生文件选择对话框以选择图片。
  ///
  /// 返回所选文件的路径，若用户取消则返回 `null`。
  Future<String?> pickImage() {
    return _impl.pickImage();
  }

  /// 获取平台版本字符串。
  Future<String?> getPlatformVersion() {
    return _impl.getPlatformVersion();
  }

  /// 释放原生资源。
  ///
  /// 当不再需要 OCR 引擎时调用此方法。
  /// 释放后若要继续使用，必须重新初始化引擎。
  void dispose() {
    if (_platform is FfiPaddleOcr) {
      (_platform as FfiPaddleOcr).dispose();
    }
    _platform = null;
  }
}
