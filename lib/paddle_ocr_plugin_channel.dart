import 'dart:developer' as developer;

import 'package:flutter/services.dart';

/// 依赖 Flutter 插件注册器的少量功能的 MethodChannel 通道。
///
/// 识别链路 100% 走 FFI（见 [FfiPaddleOcr]），不走本通道 —— 整图字节
/// 经 MethodChannel 序列化传输太慢，且会催生一套降级桩。本通道只承载
/// 两件 FFI 做不到的事：
///   * [pickImage]：原生文件对话框（Windows GetOpenFileNameW / Linux
///     GtkFileChooserDialog），需要插件注册器；
///   * [getPlatformVersion]：平台信息查询。
class PluginChannelPaddleOcr {
  /// 与原生端通信的 MethodChannel，名称为 'paddle_ocr'。
  final MethodChannel _channel = const MethodChannel('paddle_ocr');

  void _log(String message) {
    developer.log(message, name: 'PaddleOcr');
    // ignore: avoid_print
    print('[PaddleOcr] $message');
  }

  /// 打开原生文件选择对话框，返回所选图片路径（取消返回 null）。
  Future<String?> pickImage() async {
    _log('pickImage: calling native picker');
    try {
      return await _channel.invokeMethod<String>('pickImage');
    } on PlatformException catch (e) {
      _log('pickImage: PlatformException code=${e.code} message=${e.message}');
      rethrow;
    }
  }

  /// 获取平台版本字符串。
  Future<String?> getPlatformVersion() async {
    try {
      return await _channel.invokeMethod<String>('getPlatformVersion');
    } catch (e) {
      _log('getPlatformVersion: $e');
      rethrow;
    }
  }
}
