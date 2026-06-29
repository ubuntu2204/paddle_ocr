#include "pp_ocr_ffi.h"
#include "ocr_engine.h"
#include "debug_utils.h"

#include <string>
#include <vector>
#include <cstring>

// ---------------------------------------------------------------------------
// 全局版本字符串
// ---------------------------------------------------------------------------
static const char* kVersion = "pp_ocr 0.2.0";

// ---------------------------------------------------------------------------
// 引擎包装器：持有 C++ OcrEngine 实例及可复用的结果缓冲区
// ---------------------------------------------------------------------------
struct PpOcrEngine {
  paddle_ocr::OcrEngine engine;  // C++ OCR 引擎实例
  std::string last_error;        // 最近一次错误信息
  // 可复用缓冲区，避免每次调用都重新分配内存
  std::vector<PpOcrResultItem> result_items;  // 结果项数组
  std::vector<std::string> text_buffers;      // 保持文本字符串存活，防止 c_str() 指针失效
  PpOcrResultArray result_array;              // 返回给调用方的结果数组
};

// ---------------------------------------------------------------------------
// 辅助函数：将 OcrBoxResult 向量转换为 C 风格的结果数组
// ---------------------------------------------------------------------------
static PpOcrResultArray convertResults(PpOcrEngine* handle,
                                       const std::vector<paddle_ocr::OcrBoxResult>& results) {
  handle->text_buffers.clear();
  handle->result_items.clear();

  // 预留容量以防止 push_back 过程中发生重新分配。
  // 若不预留，text_buffers 可能在 push_back 时重新分配内存，导致所有
  // std::string 对象被移动，从而使已存储在 result_items 中的 c_str() 指针失效——
  // 这将导致悬空指针和 UTF-8 解码错误。
  handle->text_buffers.reserve(results.size());
  handle->result_items.reserve(results.size());

  // 第一遍：存储所有文本字符串。此循环结束后，vector 的内部存储已稳定
  //（不再有 push_back），因此 c_str() 指针保持有效。
  for (const auto& r : results) {
    handle->text_buffers.push_back(r.text);
  }

  // 第二遍：使用稳定的 c_str() 指针构建结果项
  for (size_t i = 0; i < results.size(); ++i) {
    PpOcrResultItem item;
    // 复制边界框点（4 个点，每个点包含 x, y）
    for (int j = 0; j < 4 && j < static_cast<int>(results[i].box.size()); ++j) {
      item.box[j * 2] = static_cast<double>(results[i].box[j].x);
      item.box[j * 2 + 1] = static_cast<double>(results[i].box[j].y);
    }
    // 指针稳定：text_buffers 不会再被修改，result_items 不会重新分配（已预留至精确大小）
    item.text = handle->text_buffers[i].c_str();
    item.confidence = static_cast<double>(results[i].confidence);
    handle->result_items.push_back(item);
  }

  handle->result_array.items = handle->result_items.data();
  handle->result_array.count = static_cast<int>(handle->result_items.size());
  handle->result_array.error = nullptr;
  return handle->result_array;
}

// 生成一个包含错误信息的结果数组
static PpOcrResultArray errorResult(PpOcrEngine* handle, const char* msg) {
  handle->result_array.items = nullptr;
  handle->result_array.count = 0;
  handle->result_array.error = msg;
  return handle->result_array;
}

// ---------------------------------------------------------------------------
// C API 实现
// ---------------------------------------------------------------------------

extern "C" {

// 创建新的 OCR 引擎实例
PP_OCR_EXPORT PpOcrEngine* pp_ocr_create(void) {
  return new PpOcrEngine();
}

// 销毁 OCR 引擎实例
PP_OCR_EXPORT void pp_ocr_destroy(PpOcrEngine* engine) {
  delete engine;
}

// 使用模型路径初始化 OCR 引擎
PP_OCR_EXPORT int pp_ocr_initialize(PpOcrEngine* engine,
                                      const char* det_model_path,
                                      const char* rec_model_path,
                                      const char* dict_path) {
  if (!engine || !det_model_path || !rec_model_path || !dict_path) {
    if (engine) engine->last_error = "Null argument";
    return 0;
  }

  bool ok = engine->engine.Initialize(det_model_path, rec_model_path, dict_path);
  if (!ok) {
    engine->last_error = engine->engine.GetLastError();
    if (engine->last_error.empty()) {
      engine->last_error = "Unknown initialization error";
    }
    OCR_LOG("FFI pp_ocr_initialize failed: %s", engine->last_error.c_str());
    return 0;
  }
  engine->last_error.clear();
  return 1;
}

// 检查引擎是否已初始化
PP_OCR_EXPORT int pp_ocr_is_initialized(PpOcrEngine* engine) {
  if (!engine) return 0;
  return engine->engine.IsInitialized() ? 1 : 0;
}

// 获取最近一次错误信息
PP_OCR_EXPORT const char* pp_ocr_get_last_error(PpOcrEngine* engine) {
  if (!engine) return nullptr;
  if (engine->last_error.empty()) return nullptr;
  return engine->last_error.c_str();
}

// 从图像文件识别文本
PP_OCR_EXPORT PpOcrResultArray pp_ocr_recognize_file(
    PpOcrEngine* engine, const char* image_path) {
  if (!engine) return errorResult(nullptr, "Null engine");
  if (!image_path) return errorResult(engine, "Null image_path");
  if (!engine->engine.IsInitialized()) {
    return errorResult(engine, "Engine not initialized");
  }

  OCR_LOG("FFI pp_ocr_recognize_file: path='%s'", image_path);

  auto results = engine->engine.RecognizeFromFile(image_path);
  return convertResults(engine, results);
}

// 从图像字节数据识别文本
PP_OCR_EXPORT PpOcrResultArray pp_ocr_recognize_bytes(
    PpOcrEngine* engine, const uint8_t* data, int length) {
  if (!engine) return errorResult(nullptr, "Null engine");
  if (!data || length <= 0) return errorResult(engine, "Null or empty data");
  if (!engine->engine.IsInitialized()) {
    return errorResult(engine, "Engine not initialized");
  }

  OCR_LOG("FFI pp_ocr_recognize_bytes: length=%d", length);

  std::vector<uint8_t> bytes(data, data + length);
  auto results = engine->engine.RecognizeFromBytes(bytes);
  return convertResults(engine, results);
}

// 获取 OCR 库版本字符串
PP_OCR_EXPORT const char* pp_ocr_version(void) {
  return kVersion;
}

}  // extern "C"
