#include "include/paddle_ocr/paddle_ocr_plugin_c_api.h"

#include <flutter/plugin_registrar_windows.h>

#include "paddle_ocr_plugin.h"

void PaddleOcrPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar) {
  paddle_ocr::PaddleOcrPlugin::RegisterWithRegistrar(
      flutter::PluginRegistrarManager::GetInstance()
          ->GetRegistrar<flutter::PluginRegistrarWindows>(registrar));
}
