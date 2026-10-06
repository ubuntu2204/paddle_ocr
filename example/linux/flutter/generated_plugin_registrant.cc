//
//  Generated file. Do not edit.
//

// clang-format off

#include "generated_plugin_registrant.h"

#include <pp_ocr/paddle_ocr_plugin.h>

void fl_register_plugins(FlPluginRegistry* registry) {
  g_autoptr(FlPluginRegistrar) pp_ocr_registrar =
      fl_plugin_registry_get_registrar_for_plugin(registry, "PaddleOcrPlugin");
  paddle_ocr_plugin_register_with_registrar(pp_ocr_registrar);
}
