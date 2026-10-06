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
// pickImage：优先使用 GtkFileChooserNative（可能走 xdg-desktop-portal
// 子进程，避开本进程 GL），无 portal 时自动回退到 GtkFileChooserDialog。
//
// 为什么不用 gtk_dialog_run 同同步环：它会跑一个嵌套的 GMainLoop，期间
// Flutter engine 的 messenger 也在同一个 GMainContext 上派发，可能使外层
// 的 method_call 在长时间阻塞后失效。file_picker 等成熟插件均采用
// g_signal_connect("response") 异步形式。
//
// 生命周期：ctx 由 "response" 信号统一处理（Native 与 Dialog 都提供
// "response" 信号）；对 Native，额外监听 "destroy" 兵底。
// ---------------------------------------------------------------------------
struct PickerContext {
  FlMethodCall* method_call;  // ref-owned；回应后置 nullptr 作为哨兵
  gpointer widget;            // Native (GtkFileChooserNative*) 或 Dialog
                              // (GtkWidget*)—— 统一用 gpointer 存，避免
                              // 假定的 GtkWidget/GObject 继承关系。
};

void RespondWithEmpty(FlMethodCall* method_call) {
  g_autoptr(FlValue) empty = fl_value_new_string("");
  g_autoptr(FlMethodResponse) resp =
      FL_METHOD_RESPONSE(fl_method_success_response_new(empty));
  fl_method_call_respond(method_call, resp, nullptr);
}

// 从 Native 或 Dialog 中取文件名。两者都实现了 GtkFileChooser 接口，
// 直接 cast 到 GTK_FILE_CHOOSER 即可。
static gchar* ExtractFilename(gpointer widget) {
  if (widget && GTK_IS_FILE_CHOOSER(widget)) {
    return gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(widget));
  }
  return nullptr;
}

// 统一的 "response" 处理：不管来自 GtkFileChooserDialog 还是
// GtkFileChooserNative，都走这里。GTK 将两个信号的第一个参数都传为
// 发射者本身，对 Native 它是 GObject*，对 Dialog 它是 GtkWidget*。
void OnPickerResponse(gpointer widget, gint response_id,
                       gpointer user_data) {
  auto* ctx = static_cast<PickerContext*>(user_data);
  OCR_LOG("pickImage: response_id=%d widget=%p", response_id, widget);

  if (ctx->method_call == nullptr) {
    // 已回应过，不再重复。注意：此处不再 destroy widget，
    // 避免重入；Native 会自行回收。
    return;
  }

  g_autoptr(FlValue) result = nullptr;
  if (response_id == GTK_RESPONSE_ACCEPT) {
    g_autofree gchar* filename = ExtractFilename(widget);
    if (filename) {
      OCR_LOG("pickImage: chosen='%s'", filename);
      result = fl_value_new_string(filename);
    } else {
      OCR_LOG("pickImage: ACCEPT but filename=null");
      result = fl_value_new_string("");
    }
  } else {
    OCR_LOG("pickImage: cancelled/rejected (response=%d)", response_id);
    result = fl_value_new_string("");
  }

  g_autoptr(FlMethodResponse) resp =
      FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  fl_method_call_respond(ctx->method_call, resp, nullptr);
  OCR_LOG("pickImage: responded");

  g_object_unref(ctx->method_call);
  ctx->method_call = nullptr;  // 哨兵

  // 释放 widget：
  //   • Native 需 gtk_native_dialog_destroy。
  //   • Dialog 需 gtk_widget_destroy。
  if (GTK_IS_NATIVE_DIALOG(widget)) {
    gtk_native_dialog_destroy(GTK_NATIVE_DIALOG(widget));
  } else {
    gtk_widget_destroy(GTK_WIDGET(widget));
  }
  delete ctx;
}

// 兵底：如果 widget 先被外部销毁（没走 response），确保 ctx 不泄漏、
// method_call 能被回应。
void OnPickerDestroy(GtkWidget* /*widget*/, gpointer user_data) {
  auto* ctx = static_cast<PickerContext*>(user_data);
  if (ctx == nullptr) return;
  if (ctx->method_call != nullptr) {
    OCR_LOG("pickImage: widget destroyed before response");
    RespondWithEmpty(ctx->method_call);
    g_object_unref(ctx->method_call);
    ctx->method_call = nullptr;
  }
  delete ctx;
}

void DoPickImage(PluginState* state, FlMethodCall* method_call) {
  OCR_LOG("pickImage: enter");

  // Headless 自检钩子：如果设置了 PP_OCR_PICKIMAGE_FAKE_PATH，
  // 不弹任何 dialog，直接异步地返回固定路径（或取消）。
  // 避免自动化测试需要人工点击。PP_OCR_PICKIMAGE_FAKE_PATH="" 会
  // 触发取消分支（Dart 侧拿到空串）。
  if (const char* fake = g_getenv("PP_OCR_PICKIMAGE_FAKE_PATH")) {
    OCR_LOG("pickImage: HEADLESS fake path='%s'", fake);
    // 用 idle_add 异步回应，保持与 dialog 回调同样的时序。
    struct IdleCtx { FlMethodCall* call; std::string path; };
    auto* ictx = new IdleCtx{method_call, std::string(fake)};
    g_object_ref(method_call);
    g_idle_add(
        +[](gpointer data) -> gboolean {
          auto* c = static_cast<IdleCtx*>(data);
          g_autoptr(FlValue) v = fl_value_new_string(c->path.c_str());
          g_autoptr(FlMethodResponse) r = FL_METHOD_RESPONSE(
              fl_method_success_response_new(v));
          fl_method_call_respond(c->call, r, nullptr);
          g_object_unref(c->call);
          delete c;
          return G_SOURCE_REMOVE;
        },
        ictx);
    return;
  }

  GtkWindow* parent = GetTopLevelWindow(state);
  OCR_LOG("pickImage: parent window=%p", (void*)parent);

  // 优先用 GtkFileChooserNative：在有 xdg-desktop-portal 的系统上会
  // 把选文件交给独立进程，避开本进程的 GL 驱动问题（如 Loongson
  // gf_dri.so + Impeller OpenGLESSDF 已知 segfault）。
  GtkFileChooserNative* native = gtk_file_chooser_native_new(
      "\xe9\x80\x89\xe6\x8b\xa9\xe5\x9b\xbe\xe7\x89\x87" /* 选择图片 */, parent,
      GTK_FILE_CHOOSER_ACTION_OPEN,
      "_Open", "_Cancel");

  if (native) {
    auto* ctx = new PickerContext{method_call, native};
    g_object_ref(method_call);
    g_signal_connect(native, "response", G_CALLBACK(OnPickerResponse), ctx);
    // Native 不保证销毁时发 "destroy"，但 gtk_native_dialog_destroy
    // 会回收对象；上面 response 中已经 delete ctx，不再连 destroy。
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(native));
    OCR_LOG("pickImage: GtkFileChooserNative shown");
    return;
  }

  OCR_LOG("pickImage: native chooser returned NULL, falling back to dialog");

  // 回退到旧路径：GtkFileChooserDialog
  GtkWidget* dialog = gtk_file_chooser_dialog_new(
      "\xe9\x80\x89\xe6\x8b\xa9\xe5\x9b\xbe\xe7\x89\x87" /* 选择图片 */, parent,
      GTK_FILE_CHOOSER_ACTION_OPEN,
      "_Cancel", GTK_RESPONSE_CANCEL,
      "_Open",   GTK_RESPONSE_ACCEPT,
      (const gchar*)NULL);
  if (!dialog) {
    OCR_LOG("pickImage: both native and dialog creation failed");
    RespondWithEmpty(method_call);
    return;
  }

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

  auto* ctx = new PickerContext{method_call, dialog};
  g_object_ref(method_call);
  g_signal_connect(dialog, "response", G_CALLBACK(OnPickerResponse), ctx);
  g_signal_connect(dialog, "destroy", G_CALLBACK(OnPickerDestroy), ctx);

  gtk_widget_show_all(dialog);
  OCR_LOG("pickImage: dialog shown (fallback path)");
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
