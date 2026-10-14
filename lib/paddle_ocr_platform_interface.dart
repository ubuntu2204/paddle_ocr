import 'dart:typed_data';

import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'pp_ocr_ffi.dart';
import 'src/ocr_options.dart';
import 'src/ocr_result.dart';

/// pp_ocr 插件的平台接口。
///
/// 默认实现使用 FFI（dart:ffi）直接调用原生库。
/// 当 FFI 不可用时（例如找不到 DLL），会自动回退到 MethodChannel。
abstract class PaddleOcrPlatform extends PlatformInterface {
  PaddleOcrPlatform() : super(token: _token);

  /// 平台接口令牌，用于验证子类合法性。
  static final Object _token = Object();

  // 默认：优先尝试 FFI，FFI 内部会回退到 MethodChannel。
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

  /// 设置引擎级默认选项：此后不传 options 的识别调用都会用它。
  ///
  /// 适合宿主 App 启动时按业务场景（监控、文档、车牌）配置一次，
  /// 之后所有调用点保持干净。默认实现抛 [UnimplementedError]，
  /// 由各平台实现覆盖；不支持的实现应返回 false 而不是崩溃。
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
