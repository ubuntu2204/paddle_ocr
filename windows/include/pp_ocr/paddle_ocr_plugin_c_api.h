#ifndef FLUTTER_PLUGIN_PADDLE_OCR_PLUGIN_C_API_H_
#define FLUTTER_PLUGIN_PADDLE_OCR_PLUGIN_C_API_H_

#include <flutter_plugin_registrar.h>

// 导出/导入宏定义：编译插件时导出，使用插件时导入
#ifdef FLUTTER_PLUGIN_IMPL
#define FLUTTER_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FLUTTER_PLUGIN_EXPORT __declspec(dllimport)
#endif

#if defined(__cplusplus)
extern "C" {
#endif

// 插件 C API 注册函数：将插件注册到 Flutter 桌面插件注册器
FLUTTER_PLUGIN_EXPORT void PaddleOcrPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // FLUTTER_PLUGIN_PADDLE_OCR_PLUGIN_C_API_H_
