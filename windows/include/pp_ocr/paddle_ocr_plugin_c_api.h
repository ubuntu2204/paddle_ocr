#ifndef FLUTTER_PLUGIN_PADDLE_OCR_PLUGIN_C_API_H_
#define FLUTTER_PLUGIN_PADDLE_OCR_PLUGIN_C_API_H_

#include <flutter_plugin_registrar.h>

// SCREENSHOT_CMAKE_PATCHED: removed Chinese comments (C4819) + guarded macro (C4005)
#ifndef FLUTTER_PLUGIN_EXPORT
#ifdef FLUTTER_PLUGIN_IMPL
#define FLUTTER_PLUGIN_EXPORT __declspec(dllexport)
#else
#define FLUTTER_PLUGIN_EXPORT __declspec(dllimport)
#endif
#endif

#if defined(__cplusplus)
extern "C" {
#endif

FLUTTER_PLUGIN_EXPORT void PaddleOcrPluginCApiRegisterWithRegistrar(
    FlutterDesktopPluginRegistrarRef registrar);

#if defined(__cplusplus)
}
#endif

#endif  // FLUTTER_PLUGIN_PADDLE_OCR_PLUGIN_C_API_H_
