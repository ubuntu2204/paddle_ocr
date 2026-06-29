import 'dart:typed_data';

import 'package:plugin_platform_interface/plugin_platform_interface.dart';

import 'pp_ocr_ffi.dart';
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
  Future<List<OcrResult>> recognizeImage(String imagePath);

  /// 从图片字节数据识别文字。
  Future<List<OcrResult>> recognizeImageBytes(Uint8List imageBytes);

  /// 打开文件选择对话框以选择图片，返回所选文件路径。
  Future<String?> pickImage();

  /// 获取平台版本字符串。
  Future<String?> getPlatformVersion();
}
