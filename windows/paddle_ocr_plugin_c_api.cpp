#include "include/pp_ocr/paddle_ocr_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "paddle_ocr_plugin.h"

// C API 注册入口：将插件注册到 Flutter 桌面插件注册器
void PaddleOcrPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  paddle_ocr::PaddleOcrPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
