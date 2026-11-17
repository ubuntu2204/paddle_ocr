import 'dart:io';

/// 纯 Dart（无 Flutter 引擎）环境下的 MethodChannel 替身。
///
/// Flutter 构建时条件导入会选择 `paddle_ocr_plugin_channel.dart`；
/// 识别链路本来 100% 走 FFI，后端只要不调用 [pickImage] 就不会触发这里。
class PluginChannelPaddleOcr {
  /// 原生文件对话框依赖 Flutter 插件注册器，纯 Dart 后端不可用。
  Future<String?> pickImage() async {
    throw UnsupportedError(
      'pickImage() requires a Flutter engine (MethodChannel). '
      'In a pure Dart backend, pass the image path directly to '
      'PaddleOcr.recognizeImage().',
    );
  }

  /// 返回轻量平台信息；原生库就绪时 FfiPaddleOcr 优先用 FFI 版本字符串。
  Future<String?> getPlatformVersion() async =>
      '${Platform.operatingSystem} ${Platform.operatingSystemVersion}';
}
