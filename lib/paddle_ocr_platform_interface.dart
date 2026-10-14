import 'dart:typed_data';

import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'pp_ocr_ffi.dart';
import 'src/ocr_options.dart';
import 'src/ocr_preprocessor.dart';
import 'src/ocr_result.dart';

/// pp_ocr 插件的平台接口。
///
/// 唯一实现是 [FfiPaddleOcr]（dart:ffi 直连原生库）；识别链路不存在
/// MethodChannel 回退 —— 整图字节跨通道序列化的开销不可接受。
abstract class PaddleOcrPlatform extends PlatformInterface {
  PaddleOcrPlatform() : super(token: _token);

  /// 平台接口令牌，用于验证子类合法性。
  static final Object _token = Object();

  // 默认实现。
  static PaddleOcrPlatform _instance = FfiPaddleOcr();

  /// 获取当前平台接口实例。
  static PaddleOcrPlatform get instance => _instance;

  /// 设置平台接口实例，需通过令牌验证。
  static set instance(PaddleOcrPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  /// 使用模型路径初始化 OCR 引擎。
  Future<bool> initialize({
    required String detModelPath,
    required String recModelPath,
    required String dictPath,
  });

  /// 从图片文件路径识别文字。
  ///
  /// [options] 为空时使用引擎级默认选项（见 [setDefaultOptions]），
  /// 否则用给定选项跑一次多分支预处理识别。
  Future<List<OcrResult>> recognizeImage(
    String imagePath, {
    OcrOptions? options,
  });

  /// 从图片字节数据识别文字。
  Future<List<OcrResult>> recognizeImageBytes(
    Uint8List imageBytes, {
    OcrOptions? options,
  });

  /// 用宿主注入的预处理变体跑编排管线（插件接口）。
  ///
  /// 每个预处理产出一个变体图；管线对全部变体并行检测、合并为识别窗、
  /// 再对每个窗在「底图 + 识别源变体」上择优识别。详见
  /// [OcrPreprocessor]。
  Future<List<OcrResult>> runPipeline(
    Uint8List imageBytes,
    List<OcrPreprocessor> preprocessors, {
    OcrOptions? options,
  });

  /// 设置引擎级默认选项：此后不传 options 的识别调用都会用它。
  Future<bool> setDefaultOptions(OcrOptions options) async {
    throw UnimplementedError('setDefaultOptions() is not implemented');
  }

  /// 清除引擎级默认选项，回到标准单分支行为。
  void clearDefaultOptions() {
    // 默认不做任何事：不支持该能力的平台实现保持原行为即可。
  }

  /// 打开文件选择对话框以选择图片，返回所选文件路径。
  Future<String?> pickImage();

  /// 获取平台版本字符串。
  Future<String?> getPlatformVersion();
}
