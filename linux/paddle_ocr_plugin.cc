#include "include/pp_ocr/paddle_ocr_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "debug_utils.h"
#include "ocr_engine.h"

// ---------------------------------------------------------------------------
// Plugin 状态：持有 OCR 引擎实例与宿主 GtkWindow（用于弹出对话框）。
// 生命周期由 FlMethodChannel 的 user_data 销毁回调管理。
// ---------------------------------------------------------------------------
namespace {

struct PluginState {
  std::unique_ptr<paddle_ocr::OcrEngine> engine;
  FlView* view = nullptr;  // 弱引用，由宿主 Flutter widget 树拥有

  PluginState() : engine(std::make_unique<paddle_ocr::OcrEngine>()) {}
};

// 安全地获取当前 FlView 所属的顶层 GtkWindow。返回 nullptr
// 表示对话框可以无父窗弹出。不在注册时缓存，避免注册时 FlView
// 尚未 realize / 尚未加入窗口时强转到 GtkWindow 造成 SIGSEGV。
GtkWindow* GetTopLevelWindow(PluginState* state) {
  if (!state || !GTK_IS_WIDGET(state->view)) return nullptr;
  GtkWidget* top = gtk_widget_get_toplevel(GTK_WIDGET(state->view));
  if (top && GTK_IS_WINDOW(top)) return GTK_WINDOW(top);
  return nullptr;
}

// 从 FlValue map 中安全读取字符串字段；不存在或类型不匹配时返回空串。
std::string GetArgString(FlValue* args, const char* key) {
  if (!args || fl_value_get_type(args) != FL_VALUE_TYPE_MAP) return "";
  g_autoptr(FlValue) key_val = fl_value_new_string(key);
  FlValue* v = fl_value_lookup(args, key_val);
  if (!v || fl_value_get_type(v) != FL_VALUE_TYPE_STRING) return "";
  const gchar* s = fl_value_get_string(v);
  return s ? std::string(s) : std::string();
}

// 将 OcrBoxResult 列表编码为 FlValue 列表（每项是 { box, text, confidence }）。
// 返回的 FlValue 由调用方负责释放（g_autoptr）。
// 注意：Flutter Linux 的 FlValue API 使用 fl_value_new_float 表示 double，
// 并使用 fl_value_set_string_take 避免堆分配 key。
FlValue* ResultsToFlValue(
    const std::vector<paddle_ocr::OcrBoxResult>& results) {
  FlValue* arr = fl_value_new_list();
  for (const auto& r : results) {
    FlValue* entry_map = fl_value_new_map();

    FlValue* box_list = fl_value_new_list();
    for (const auto& pt : r.box) {
      FlValue* pt_list = fl_value_new_list();
      fl_value_append_take(pt_list,
                           fl_value_new_float(static_cast<double>(pt.x)));
      fl_value_append_take(pt_list,
                           fl_value_new_float(static_cast<double>(pt.y)));
      fl_value_append_take(box_list, pt_list);
    }
    fl_value_set_string_take(entry_map, "box", box_list);

    fl_value_set_string_take(entry_map, "text",
                             fl_value_new_string(r.text.c_str()));

    fl_value_set_string_take(entry_map, "confidence",
                             fl_value_new_float(
                                 static_cast<double>(r.confidence)));

    fl_value_append_take(arr, entry_map);
  }
  return arr;
}

// ---------------------------------------------------------------------------
// pickImage：使用 GtkFileChooserDialog 阻塞式弹出文件选择框。
// ---------------------------------------------------------------------------
void DoPickImage(PluginState* state, FlMethodCall* method_call) {
  OCR_LOG("pickImage: enter");
  GtkWindow* parent = GetTopLevelWindow(state);
  OCR_LOG("pickImage: parent window=%p", (void*)parent);

  // 注意：GTK 的变参列表以 (const gchar*)NULL 终止；C++ 的 nullptr
  // 属于 std::nullptr_t，不是指针类型，在变参上下文属 UB。
  GtkWidget* dialog = gtk_file_chooser_dialog_new(
      "\xe9\x80\x89\xe6\x8b\xa9\xe5\x9b\xbe\xe7\x89\x87" /* 选择图片 */, parent,
      GTK_FILE_CHOOSER_ACTION_OPEN,
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_Open",   GTK_RESPONSE_ACCEPT,
      (const gchar*)NULL);
  if (!dialog) {
    OCR_LOG("pickImage: gtk_file_chooser_dialog_new returned NULL");
    g_autoptr(FlValue) empty = fl_value_new_string("");
    g_autoptr(FlMethodResponse) resp =
        FL_METHOD_RESPONSE(fl_method_success_response_new(empty));
    fl_method_call_respond(method_call, resp, nullptr);
    return;
  }

  // 图片过滤器
  GtkFileFilter* img_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(img_filter, "Images");
  for (const char* pat : {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tif",
                          "*.tiff", "*.webp"}) {
    gtk_file_filter_add_pattern(img_filter, pat);
  }
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), img_filter);
  g_object_unref(img_filter);

  GtkFileFilter* all_filter = gtk_file_filter_new();
  gtk_file_filter_set_name(all_filter, "All Files");
  gtk_file_filter_add_pattern(all_filter, "*");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), all_filter);
  g_object_unref(all_filter);

  OCR_LOG("pickImage: running modal dialog...");
  gint response = gtk_dialog_run(GTK_DIALOG(dialog));
  OCR_LOG("pickImage: dialog response=%d", response);

  g_autoptr(FlValue) result = nullptr;
  if (response == GTK_RESPONSE_ACCEPT) {
    g_autofree gchar* filename =
        gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    result = fl_value_new_string(filename ? filename : "");
    if (filename) OCR_LOG("pickImage: chosen='%s'", filename);
  } else {
    result = fl_value_new_string("");
  }
  gtk_widget_destroy(dialog);

  g_autoptr(FlMethodResponse) resp =
      FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  fl_method_call_respond(method_call, resp, nullptr);
  OCR_LOG("pickImage: responded");
}

// ---------------------------------------------------------------------------
// MethodChannel 回调：分发 Dart 侧调用。
// ---------------------------------------------------------------------------
void HandleMethodCall(FlMethodChannel* /*channel*/, FlMethodCall* method_call,
                      gpointer user_data) {
  auto* state = static_cast<PluginState*>(user_data);
  const gchar* method = fl_method_call_get_name(method_call);
  FlValue* args = fl_method_call_get_args(method_call);

  OCR_LOG("HandleMethodCall: method='%s'", method);

  auto respond_error = [&](const char* code, const char* message) {
    g_autoptr(FlMethodResponse) resp = FL_METHOD_RESPONSE(
        fl_method_error_response_new(code, message, nullptr));
    fl_method_call_respond(method_call, resp, nullptr);
  };

  auto respond_bool = [&](bool value) {
    g_autoptr(FlValue) v = fl_value_new_bool(value ? TRUE : FALSE);
    g_autoptr(FlMethodResponse) resp =
        FL_METHOD_RESPONSE(fl_method_success_response_new(v));
    fl_method_call_respond(method_call, resp, nullptr);
  };

  auto respond_string = [&](const std::string& value) {
    g_autoptr(FlValue) v = fl_value_new_string(value.c_str());
    g_autoptr(FlMethodResponse) resp =
        FL_METHOD_RESPONSE(fl_method_success_response_new(v));
    fl_method_call_respond(method_call, resp, nullptr);
  };

  auto respond_results =
      [&](const std::vector<paddle_ocr::OcrBoxResult>& results) {
        g_autoptr(FlValue) v = ResultsToFlValue(results);
        g_autoptr(FlMethodResponse) resp =
            FL_METHOD_RESPONSE(fl_method_success_response_new(v));
        fl_method_call_respond(method_call, resp, nullptr);
      };

  if (g_strcmp0(method, "initialize") == 0) {
    std::string det = GetArgString(args, "detModelPath");
    std::string rec = GetArgString(args, "recModelPath");
    std::string dict = GetArgString(args, "dictPath");
    if (det.empty() || rec.empty() || dict.empty()) {
      respond_error("INVALID_ARGS",
                    "detModelPath, recModelPath, and dictPath are required");
      return;
    }
    OCR_LOG("initialize: det='%s' rec='%s' dict='%s'", det.c_str(), rec.c_str(),
            dict.c_str());
    bool ok = state->engine->Initialize(det, rec, dict);
    if (ok) {
      respond_bool(true);
    } else {
      std::string err = "Failed to initialize OCR engine";
      const std::string& detail = state->engine->GetLastError();
      if (!detail.empty()) err += ": " + detail;
      respond_error("INIT_FAILED", err.c_str());
    }
    return;
  }

  if (g_strcmp0(method, "recognizeImage") == 0) {
    if (!state->engine->IsInitialized()) {
      respond_error("NOT_INITIALIZED",
                    "OCR engine not initialized. Call initialize() first.");
      return;
    }
    std::string path = GetArgString(args, "imagePath");
    if (path.empty()) {
      respond_error("INVALID_ARGS", "imagePath is required");
      return;
    }
    OCR_LOG("recognizeImage: path='%s'", path.c_str());
    auto results = state->engine->RecognizeFromFile(path);
    respond_results(results);
    return;
  }

  if (g_strcmp0(method, "recognizeImageBytes") == 0) {
    if (!state->engine->IsInitialized()) {
      respond_error("NOT_INITIALIZED",
                    "OCR engine not initialized. Call initialize() first.");
      return;
    }
    if (!args || fl_value_get_type(args) != FL_VALUE_TYPE_MAP) {
      respond_error("INVALID_ARGS", "Arguments required");
      return;
    }
    g_autoptr(FlValue) key_val = fl_value_new_string("imageBytes");
    FlValue* v_bytes = fl_value_lookup(args, key_val);
    if (!v_bytes || fl_value_get_type(v_bytes) != FL_VALUE_TYPE_UINT8_LIST) {
      respond_error("INVALID_ARGS", "imageBytes is required");
      return;
    }
    size_t len = fl_value_get_length(v_bytes);
    const uint8_t* data = fl_value_get_uint8_list(v_bytes);
    std::vector<uint8_t> bytes(data, data + len);
    OCR_LOG("recognizeImageBytes: %zu bytes", bytes.size());
    auto results = state->engine->RecognizeFromBytes(bytes);
    respond_results(results);
    return;
  }

  if (g_strcmp0(method, "pickImage") == 0) {
    DoPickImage(state, method_call);
    return;
  }

  if (g_strcmp0(method, "getPlatformVersion") == 0) {
    respond_string("Linux");
    return;
  }

  g_autoptr(FlMethodResponse) resp = FL_METHOD_RESPONSE(
      fl_method_not_implemented_response_new());
  fl_method_call_respond(method_call, resp, nullptr);
}

void DestroyState(gpointer data) {
  delete static_cast<PluginState*>(data);
}

}  // namespace

// ---------------------------------------------------------------------------
// 注册入口（C 符号，由 Flutter 生成的 registrant 调用）
// ---------------------------------------------------------------------------
extern "C" FLUTTER_PLUGIN_EXPORT void paddle_ocr_plugin_register_with_registrar(
    FlPluginRegistrar* registrar) {
  FlBinaryMessenger* messenger =
      fl_plugin_registrar_get_messenger(registrar);
  FlView* view = fl_plugin_registrar_get_view(registrar);

  auto* state = new PluginState();
  // 保存 FlView 弱引用；实际 GtkWindow 在 pickImage 时懒查找。
  state->view = view;

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  g_autoptr(FlMethodChannel) channel = fl_method_channel_new(
      messenger, "paddle_ocr", FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(channel, HandleMethodCall, state,
                                            DestroyState);
}
