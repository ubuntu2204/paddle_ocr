#ifndef FLUTTER_PLUGIN_PADDLE_OCR_PLUGIN_H_
#define FLUTTER_PLUGIN_PADDLE_OCR_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>

#include "ocr_engine.h"

namespace paddle_ocr {

// Flutter MethodChannel 插件类：处理来自 Dart 侧的方法调用
class PaddleOcrPlugin : public flutter::Plugin {
 public:
  // 向 Flutter 注册器注册插件
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  PaddleOcrPlugin();
  virtual ~PaddleOcrPlugin();

  // 禁用拷贝构造和赋值
  PaddleOcrPlugin(const PaddleOcrPlugin&) = delete;
  PaddleOcrPlugin& operator=(const PaddleOcrPlugin&) = delete;

 private:
  // 处理来自 Dart 侧的 MethodChannel 方法调用
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  std::unique_ptr<OcrEngine> ocr_engine_;  // OCR 引擎实例
};

}  // namespace paddle_ocr

#endif  // FLUTTER_PLUGIN_PADDLE_OCR_PLUGIN_H_
