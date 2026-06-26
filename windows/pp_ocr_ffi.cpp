#include "pp_ocr_ffi.h"
#include "ocr_engine.h"
#include "debug_utils.h"

#include <string>
#include <vector>
#include <cstring>

// ---------------------------------------------------------------------------
// Global version string
// ---------------------------------------------------------------------------
static const char* kVersion = "pp_ocr 0.2.0";

// ---------------------------------------------------------------------------
// Engine wrapper: holds the C++ OcrEngine + reusable result buffer
// ---------------------------------------------------------------------------
struct PpOcrEngine {
  paddle_ocr::OcrEngine engine;
  std::string last_error;
  // Reusable buffers to avoid per-call allocation
  std::vector<PpOcrResultItem> result_items;
  std::vector<std::string> text_buffers;  // keep text strings alive
  PpOcrResultArray result_array;
};

// ---------------------------------------------------------------------------
// Helper: Convert OcrBoxResult vector to C result array
// ---------------------------------------------------------------------------
static PpOcrResultArray convertResults(PpOcrEngine* handle,
                                       const std::vector<paddle_ocr::OcrBoxResult>& results) {
  handle->text_buffers.clear();
  handle->result_items.clear();
  handle->result_items.reserve(results.size());

  for (const auto& r : results) {
    PpOcrResultItem item;
    // Copy box points (4 points, each x,y)
    for (int i = 0; i < 4 && i < static_cast<int>(r.box.size()); ++i) {
      item.box[i * 2] = static_cast<double>(r.box[i].x);
      item.box[i * 2 + 1] = static_cast<double>(r.box[i].y);
    }
    // Store text in persistent buffer
    handle->text_buffers.push_back(r.text);
    item.text = handle->text_buffers.back().c_str();
    item.confidence = static_cast<double>(r.confidence);
    handle->result_items.push_back(item);
  }

  handle->result_array.items = handle->result_items.data();
  handle->result_array.count = static_cast<int>(handle->result_items.size());
  handle->result_array.error = nullptr;
  return handle->result_array;
}

static PpOcrResultArray errorResult(PpOcrEngine* handle, const char* msg) {
  handle->result_array.items = nullptr;
  handle->result_array.count = 0;
  handle->result_array.error = msg;
  return handle->result_array;
}

// ---------------------------------------------------------------------------
// C API implementation
// ---------------------------------------------------------------------------

extern "C" {

PP_OCR_EXPORT PpOcrEngine* pp_ocr_create(void) {
  return new PpOcrEngine();
}

PP_OCR_EXPORT void pp_ocr_destroy(PpOcrEngine* engine) {
  delete engine;
}

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

PP_OCR_EXPORT int pp_ocr_is_initialized(PpOcrEngine* engine) {
  if (!engine) return 0;
  return engine->engine.IsInitialized() ? 1 : 0;
}

PP_OCR_EXPORT const char* pp_ocr_get_last_error(PpOcrEngine* engine) {
  if (!engine) return nullptr;
  if (engine->last_error.empty()) return nullptr;
  return engine->last_error.c_str();
}

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

PP_OCR_EXPORT const char* pp_ocr_version(void) {
  return kVersion;
}

}  // extern "C"
