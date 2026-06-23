#ifndef FLUTTER_PLUGIN_PADDLE_OCR_PLUGIN_H_
#define FLUTTER_PLUGIN_PADDLE_OCR_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>

#include <memory>

#include "ocr_engine.h"

namespace paddle_ocr {

class PaddleOcrPlugin : public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrarWindows *registrar);

  PaddleOcrPlugin();
  virtual ~PaddleOcrPlugin();

  PaddleOcrPlugin(const PaddleOcrPlugin&) = delete;
  PaddleOcrPlugin& operator=(const PaddleOcrPlugin&) = delete;

 private:
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  std::unique_ptr<OcrEngine> ocr_engine_;
};

}  // namespace paddle_ocr

#endif  // FLUTTER_PLUGIN_PADDLE_OCR_PLUGIN_H_
