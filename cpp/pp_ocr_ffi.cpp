#include "pp_ocr_ffi.h"
#include "ocr_engine.h"
#include "debug_utils.h"

#include <cmath>
#include <string>
#include <vector>
#include <cstring>

// ---------------------------------------------------------------------------
// 全局版本字符串
// ---------------------------------------------------------------------------
// 与根 pubspec.yaml 的 version 保持同步；example 界面会显示该字符串，
// 用于核对 Dart 包与原生库的版本是否配套。
static const char* kVersion = "pp_ocr 0.5.1";

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

  // 原子操作的可复用缓冲区（下一次调用前有效）。
  std::vector<uint8_t> image_buffer;        // preprocess 输出（PNG 字节）
  std::vector<PpOcrBoxInput> box_buffer;    // detect/merge 输出
  std::string single_text_buffer;           // recognize_box 输出
  PpOcrBoxArray box_array{};                // 返回给调用方的框数组
  PpOcrImageBuffer image_out{};             // 返回给调用方的图像缓冲
  PpOcrTextResult text_out{};               // 返回给调用方的文本结果
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
// handle 可以为 nullptr（代表“创建前就失败”），此时用一个空的
// 栈上 PpOcrResultArray 返回，避免对空 handle 解引用。
static PpOcrResultArray errorResult(PpOcrEngine* handle, const char* msg) {
  PpOcrResultArray out;
  out.items = nullptr;
  out.count = 0;
  out.error = msg;
  if (handle) {
    // 仍然将错误写回 handle（便于后续查询），然后返回。
    handle->result_array = out;
    return handle->result_array;
  }
  return out;
}

// ---------------------------------------------------------------------------
// PpOcrOptions（C 平铺） <-> paddle_ocr::OcrOptions（C++ 嵌套）
//
// 两边的字段映射完全靠名字逐字段赋值，不依赖内存布局，
// 因此 C++ 侧结构体可以自由拆并嵌套层级而不破坏 ABI。
// ---------------------------------------------------------------------------
static void OptionsFromC(const PpOcrOptions& c, paddle_ocr::OcrOptions* out) {
  paddle_ocr::FillDefaultOptions(out);

  out->det.thresh = c.det_thresh;
  out->det.box_thresh = c.det_box_thresh;
  out->det.unclip_ratio = c.det_unclip_ratio;
  out->det.max_side_len = c.det_max_side_len;
  out->det.min_box_size = c.det_min_box_size;
  out->det.min_box_area = c.det_min_box_area;

  out->rec.img_height = c.rec_img_height;
  out->rec.max_img_width = c.rec_max_img_width;
  out->rec.enhance_crops = c.rec_enhance_crops;
  out->rec.try_all_branches = c.rec_try_all_branches;
  out->rec.min_confidence = c.rec_min_confidence;
  out->rec.variant_select = c.rec_variant_select;

  paddle_ocr::PreprocessParams& cp = out->rec.crop_preprocess;
  cp.binarize_mode = static_cast<paddle_ocr::BinarizeMode>(c.crop_binarize_mode);
  cp.adaptive_block_size = c.crop_adaptive_block_size;
  cp.adaptive_c = c.crop_adaptive_c;
  cp.enhance_mode = static_cast<paddle_ocr::EnhanceMode>(c.crop_enhance_mode);
  cp.clahe_clip_limit = c.crop_clahe_clip_limit;
  cp.clahe_grid_size = c.crop_clahe_grid_size;
  cp.sharpen_amount = c.crop_sharpen_amount;
  cp.morph_close_kernel = c.crop_morph_close_kernel;
  cp.morph_open_kernel = c.crop_morph_open_kernel;
  cp.color_mode = static_cast<paddle_ocr::ColorMode>(c.crop_color_mode);

  out->merge.branches = c.branches;
  out->merge.merge_iou_threshold = c.merge_iou_threshold;
  out->merge.merge_contain_threshold = c.merge_contain_threshold;
  out->merge.stitch_same_line = c.stitch_same_line;
  out->merge.line_min_y_overlap = c.line_min_y_overlap;
  out->merge.line_max_gap_ratio = c.line_max_gap_ratio;
  out->merge.line_max_height_diff_ratio = c.line_max_height_diff_ratio;
  out->merge.line_max_width_ratio = c.line_max_width_ratio;
  out->merge.region_pad_ratio = c.region_pad_ratio;
  out->merge.region_pad_long_ratio = c.region_pad_long_ratio;
  out->merge.max_regions = c.max_regions;
  out->merge.sort_order = c.sort_order;

  out->roi_x = c.roi_x;
  out->roi_y = c.roi_y;
  out->roi_w = c.roi_w;
  out->roi_h = c.roi_h;
  out->verbose_stats = c.verbose_stats;
}

static void OptionsToC(const paddle_ocr::OcrOptions& o, PpOcrOptions* out) {
  std::memset(out, 0, sizeof(PpOcrOptions));
  out->struct_size = static_cast<int32_t>(sizeof(PpOcrOptions));

  out->det_thresh = o.det.thresh;
  out->det_box_thresh = o.det.box_thresh;
  out->det_unclip_ratio = o.det.unclip_ratio;
  out->det_max_side_len = o.det.max_side_len;
  out->det_min_box_size = o.det.min_box_size;
  out->det_min_box_area = o.det.min_box_area;

  out->rec_img_height = o.rec.img_height;
  out->rec_max_img_width = o.rec.max_img_width;
  out->rec_enhance_crops = o.rec.enhance_crops;
  out->rec_try_all_branches = o.rec.try_all_branches;
  out->rec_min_confidence = o.rec.min_confidence;
  out->rec_variant_select = o.rec.variant_select;

  const paddle_ocr::PreprocessParams& cp = o.rec.crop_preprocess;
  out->crop_binarize_mode = static_cast<int32_t>(cp.binarize_mode);
  out->crop_adaptive_block_size = cp.adaptive_block_size;
  out->crop_adaptive_c = cp.adaptive_c;
  out->crop_enhance_mode = static_cast<int32_t>(cp.enhance_mode);
  out->crop_clahe_clip_limit = cp.clahe_clip_limit;
  out->crop_clahe_grid_size = cp.clahe_grid_size;
  out->crop_sharpen_amount = cp.sharpen_amount;
  out->crop_morph_close_kernel = cp.morph_close_kernel;
  out->crop_morph_open_kernel = cp.morph_open_kernel;
  out->crop_color_mode = static_cast<int32_t>(cp.color_mode);

  out->branches = o.merge.branches;
  out->merge_iou_threshold = o.merge.merge_iou_threshold;
  out->merge_contain_threshold = o.merge.merge_contain_threshold;
  out->stitch_same_line = o.merge.stitch_same_line;
  out->line_min_y_overlap = o.merge.line_min_y_overlap;
  out->line_max_gap_ratio = o.merge.line_max_gap_ratio;
  out->line_max_height_diff_ratio = o.merge.line_max_height_diff_ratio;
  out->line_max_width_ratio = o.merge.line_max_width_ratio;
  out->region_pad_ratio = o.merge.region_pad_ratio;
  out->region_pad_long_ratio = o.merge.region_pad_long_ratio;
  out->max_regions = o.merge.max_regions;
  out->sort_order = o.merge.sort_order;

  out->roi_x = o.roi_x;
  out->roi_y = o.roi_y;
  out->roi_w = o.roi_w;
  out->roi_h = o.roi_h;
  out->verbose_stats = o.verbose_stats;
}

// 校验并转换；失败时返回 false 并把原因写入 engine（可为 null）。
static bool ParseOptions(PpOcrEngine* engine, const PpOcrOptions* in,
                         paddle_ocr::OcrOptions* out) {
  if (!in) {
    if (engine) engine->last_error = "Null options";
    return false;
  }
  if (in->struct_size != static_cast<int32_t>(sizeof(PpOcrOptions))) {
    if (engine) {
      engine->last_error =
          "PpOcrOptions.struct_size mismatch: got " +
          std::to_string(in->struct_size) + ", expected " +
          std::to_string(static_cast<int32_t>(sizeof(PpOcrOptions))) +
          ". Fill it with pp_ocr_options_init()/preset().";
    }
    return false;
  }
  OptionsFromC(*in, out);
  paddle_ocr::SanitizeOptions(out);
  return true;
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
  OCR_LOG("FFI pp_ocr_initialize: ENTER");
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
  OCR_LOG("FFI pp_ocr_initialize: EXIT ok=1");
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
  OCR_LOG("FFI pp_ocr_recognize_file: ENTER path='%s'",
          image_path ? image_path : "(null)");
  if (!engine) return errorResult(nullptr, "Null engine");
  if (!image_path) return errorResult(engine, "Null image_path");
  if (!engine->engine.IsInitialized()) {
    return errorResult(engine, "Engine not initialized");
  }

  auto results = engine->engine.RecognizeFromFile(image_path);
  OCR_LOG("FFI pp_ocr_recognize_file: got %zu results, converting", results.size());
  auto out = convertResults(engine, results);
  OCR_LOG("FFI pp_ocr_recognize_file: EXIT count=%d", out.count);
  return out;
}

// 从图像字节数据识别文本
PP_OCR_EXPORT PpOcrResultArray pp_ocr_recognize_bytes(
    PpOcrEngine* engine, const uint8_t* data, int length) {
  OCR_LOG("FFI pp_ocr_recognize_bytes: ENTER length=%d",
          length);
  if (!engine) return errorResult(nullptr, "Null engine");
  if (!data || length <= 0) return errorResult(engine, "Null or empty data");
  if (!engine->engine.IsInitialized()) {
    return errorResult(engine, "Engine not initialized");
  }

  std::vector<uint8_t> bytes(data, data + length);
  auto results = engine->engine.RecognizeFromBytes(bytes);
  OCR_LOG("FFI pp_ocr_recognize_bytes: got %zu results, converting", results.size());
  auto out = convertResults(engine, results);
  OCR_LOG("FFI pp_ocr_recognize_bytes: EXIT count=%d", out.count);
  return out;
}

// 获取 OCR 库版本字符串
PP_OCR_EXPORT const char* pp_ocr_version(void) {
  return kVersion;
}

// ---------------------------------------------------------------------------
// 选项相关
// ---------------------------------------------------------------------------

PP_OCR_EXPORT int32_t pp_ocr_options_size(void) {
  return static_cast<int32_t>(sizeof(PpOcrOptions));
}

PP_OCR_EXPORT void pp_ocr_options_init(PpOcrOptions* out) {
  if (!out) return;
  paddle_ocr::OcrOptions o;
  paddle_ocr::FillDefaultOptions(&o);
  OptionsToC(o, out);
}

PP_OCR_EXPORT int pp_ocr_options_preset(int32_t preset_id, PpOcrOptions* out) {
  if (!out) return 0;
  const paddle_ocr::OcrOptions o = paddle_ocr::MakePresetOptions(preset_id);
  OptionsToC(o, out);
  return 1;
}

PP_OCR_EXPORT const char* pp_ocr_preset_name(int32_t preset_id) {
  return paddle_ocr::PresetName(preset_id);
}

PP_OCR_EXPORT int32_t pp_ocr_options_sanitize(PpOcrOptions* options) {
  if (!options) return -1;
  if (options->struct_size != static_cast<int32_t>(sizeof(PpOcrOptions))) {
    return -1;  // 布局不匹配时不能动它
  }
  paddle_ocr::OcrOptions o;
  OptionsFromC(*options, &o);
  const int32_t fixed = paddle_ocr::SanitizeOptions(&o);
  OptionsToC(o, options);
  return fixed;
}

PP_OCR_EXPORT int pp_ocr_set_default_options(PpOcrEngine* engine,
                                             const PpOcrOptions* options) {
  if (!engine) return 0;
  paddle_ocr::OcrOptions o;
  if (!ParseOptions(engine, options, &o)) {
    OCR_LOG("FFI pp_ocr_set_default_options failed: %s",
            engine->last_error.c_str());
    return 0;
  }
  engine->engine.SetDefaultOptions(o);
  engine->last_error.clear();
  return 1;
}

PP_OCR_EXPORT void pp_ocr_clear_default_options(PpOcrEngine* engine) {
  if (!engine) return;
  engine->engine.ClearDefaultOptions();
}

PP_OCR_EXPORT PpOcrResultArray pp_ocr_recognize_file_ex(
    PpOcrEngine* engine, const char* image_path, const PpOcrOptions* options) {
  OCR_LOG("FFI pp_ocr_recognize_file_ex: ENTER path='%s' options=%s",
          image_path ? image_path : "(null)", options ? "given" : "null");
  if (!engine) return errorResult(nullptr, "Null engine");
  if (!image_path) return errorResult(engine, "Null image_path");

  // 先校验调用方传来的结构体：布局不匹配是编程错误，必须比「引擎未
  // 初始化」更早报出来，否则开发者要先接上模型才能发现自己的用法错了。
  paddle_ocr::OcrOptions parsed;
  if (options && !ParseOptions(engine, options, &parsed)) {
    return errorResult(engine, engine->last_error.c_str());
  }
  if (!engine->engine.IsInitialized()) {
    return errorResult(engine, "Engine not initialized");
  }

  // options == nullptr 时沿用引擎级默认选项，与老接口行为一致。
  std::vector<paddle_ocr::OcrBoxResult> results;
  if (options) {
    results = engine->engine.RecognizeFromFile(image_path, parsed);
  } else {
    results = engine->engine.RecognizeFromFile(image_path);
  }

  OCR_LOG("FFI pp_ocr_recognize_file_ex: got %zu results", results.size());
  return convertResults(engine, results);
}

PP_OCR_EXPORT PpOcrResultArray pp_ocr_recognize_bytes_ex(
    PpOcrEngine* engine, const uint8_t* data, int length,
    const PpOcrOptions* options) {
  OCR_LOG("FFI pp_ocr_recognize_bytes_ex: ENTER length=%d options=%s", length,
          options ? "given" : "null");
  if (!engine) return errorResult(nullptr, "Null engine");
  if (!data || length <= 0) return errorResult(engine, "Null or empty data");

  paddle_ocr::OcrOptions parsed;
  if (options && !ParseOptions(engine, options, &parsed)) {
    return errorResult(engine, engine->last_error.c_str());
  }
  if (!engine->engine.IsInitialized()) {
    return errorResult(engine, "Engine not initialized");
  }

  std::vector<uint8_t> bytes(data, data + length);
  std::vector<paddle_ocr::OcrBoxResult> results;
  if (options) {
    results = engine->engine.RecognizeFromBytes(bytes, parsed);
  } else {
    results = engine->engine.RecognizeFromBytes(bytes);
  }

  OCR_LOG("FFI pp_ocr_recognize_bytes_ex: got %zu results", results.size());
  return convertResults(engine, results);
}

// ---------------------------------------------------------------------------
// 原子操作：宿主编排自定义识别管线
// ---------------------------------------------------------------------------

// 解码输入字节为 cv::Mat（共享实现）。
static bool DecodeInput(PpOcrEngine* engine, const uint8_t* data, int length,
                        cv::Mat* out) {
  if (!engine) return false;
  if (!data || length <= 0) {
    engine->last_error = "Null or empty data";
    return false;
  }
  *out = paddle_ocr::OcrEngine::DecodeImageBytes(
      std::vector<uint8_t>(data, data + length));
  if (out->empty()) {
    engine->last_error = "cv::imdecode failed (corrupt or unsupported image)";
    return false;
  }
  return true;
}

PP_OCR_EXPORT int32_t pp_ocr_transform_params_size(void) {
  return static_cast<int32_t>(sizeof(PpOcrTransformParams));
}

PP_OCR_EXPORT void pp_ocr_transform_params_init(PpOcrTransformParams* out) {
  if (!out) return;
  std::memset(out, 0, sizeof(PpOcrTransformParams));
  out->struct_size = static_cast<int32_t>(sizeof(PpOcrTransformParams));
  paddle_ocr::PreprocessParams p;
  paddle_ocr::FillDefaultPreprocess(&p);
  out->scale_percent = p.scale_percent;
  out->binarize_mode = static_cast<int32_t>(p.binarize_mode);
  out->adaptive_block_size = p.adaptive_block_size;
  out->adaptive_c = p.adaptive_c;
  out->enhance_mode = static_cast<int32_t>(p.enhance_mode);
  out->clahe_clip_limit = p.clahe_clip_limit;
  out->clahe_grid_size = p.clahe_grid_size;
  out->sharpen_amount = p.sharpen_amount;
  out->morph_open_kernel = p.morph_open_kernel;
  out->morph_close_kernel = p.morph_close_kernel;
  out->color_mode = static_cast<int32_t>(p.color_mode);
}

PP_OCR_EXPORT int pp_ocr_preprocess_bytes(PpOcrEngine* engine,
                                          const uint8_t* data, int length,
                                          const PpOcrTransformParams* params,
                                          PpOcrImageBuffer* out) {
  if (!engine) return 0;
  if (!out) {
    engine->last_error = "Null out buffer";
    return 0;
  }
  if (!params ||
      params->struct_size !=
          static_cast<int32_t>(sizeof(PpOcrTransformParams))) {
    engine->last_error =
        "PpOcrTransformParams.struct_size mismatch; fill it with "
        "pp_ocr_transform_params_init()";
    return 0;
  }
  cv::Mat image;
  if (!DecodeInput(engine, data, length, &image)) return 0;
  if (!engine->engine.IsInitialized()) {
    engine->last_error = "Engine not initialized";
    return 0;
  }

  // 平铺参数 → C++ 结构（合法性由 ApplyImageTransform 内部兜底）。
  paddle_ocr::PreprocessParams p;
  paddle_ocr::FillDefaultPreprocess(&p);
  p.scale_percent = params->scale_percent;
  p.binarize_mode = static_cast<paddle_ocr::BinarizeMode>(params->binarize_mode);
  p.adaptive_block_size = params->adaptive_block_size;
  p.adaptive_c = params->adaptive_c;
  p.enhance_mode = static_cast<paddle_ocr::EnhanceMode>(params->enhance_mode);
  p.clahe_clip_limit = params->clahe_clip_limit;
  p.clahe_grid_size = params->clahe_grid_size;
  p.sharpen_amount = params->sharpen_amount;
  p.morph_open_kernel = params->morph_open_kernel;
  p.morph_close_kernel = params->morph_close_kernel;
  p.color_mode = static_cast<paddle_ocr::ColorMode>(params->color_mode);

  cv::Mat transformed = engine->engine.ApplyImageTransform(image, p);
  if (transformed.empty()) {
    engine->last_error = "transform produced empty image";
    return 0;
  }
  if (!cv::imencode(".png", transformed, engine->image_buffer)) {
    engine->last_error = "cv::imencode failed";
    return 0;
  }
  engine->image_out.data = engine->image_buffer.data();
  engine->image_out.size =
      static_cast<int32_t>(engine->image_buffer.size());
  *out = engine->image_out;
  engine->last_error.clear();
  return 1;
}

PP_OCR_EXPORT int pp_ocr_detect_bytes(PpOcrEngine* engine,
                                      const uint8_t* data, int length,
                                      const PpOcrOptions* options,
                                      PpOcrBoxArray* out) {
  if (!engine) return 0;
  if (!out) {
    engine->last_error = "Null out buffer";
    return 0;
  }
  cv::Mat image;
  if (!DecodeInput(engine, data, length, &image)) return 0;
  if (!engine->engine.IsInitialized()) {
    engine->last_error = "Engine not initialized";
    return 0;
  }

  paddle_ocr::OcrOptions parsed;
  if (options) {
    if (!ParseOptions(engine, options, &parsed)) return 0;
  } else {
    paddle_ocr::FillDefaultOptions(&parsed);
  }

  auto boxes = engine->engine.DetectOnImage(image, parsed.det);
  engine->box_buffer.clear();
  engine->box_buffer.reserve(boxes.size());
  for (const auto& db : boxes) {
    PpOcrBoxInput item{};
    for (int j = 0; j < 4 && j < static_cast<int>(db.points.size()); ++j) {
      item.box[j * 2] = static_cast<double>(db.points[j].x);
      item.box[j * 2 + 1] = static_cast<double>(db.points[j].y);
    }
    item.branch_index = db.branch_index;
    item.score = db.score;
    engine->box_buffer.push_back(item);
  }
  engine->box_array.items = engine->box_buffer.data();
  engine->box_array.count = static_cast<int32_t>(engine->box_buffer.size());
  engine->box_array.image_width = image.cols;
  engine->box_array.image_height = image.rows;
  engine->box_array.error = nullptr;
  *out = engine->box_array;
  engine->last_error.clear();
  return 1;
}

PP_OCR_EXPORT int pp_ocr_merge_boxes(PpOcrEngine* engine,
                                     const PpOcrBoxInput* boxes, int count,
                                     const PpOcrOptions* options,
                                     int image_width, int image_height,
                                     PpOcrBoxArray* out) {
  if (!engine) return 0;
  if (!out) {
    engine->last_error = "Null out buffer";
    return 0;
  }
  if (count > 0 && !boxes) {
    engine->last_error = "Null boxes";
    return 0;
  }
  if (!engine->engine.IsInitialized()) {
    engine->last_error = "Engine not initialized";
    return 0;
  }

  paddle_ocr::OcrOptions parsed;
  if (options) {
    if (!ParseOptions(engine, options, &parsed)) return 0;
  } else {
    paddle_ocr::FillDefaultOptions(&parsed);
  }

  std::vector<paddle_ocr::DetectedBox> detected;
  detected.reserve(static_cast<size_t>(std::max(0, count)));
  for (int i = 0; i < count; ++i) {
    paddle_ocr::DetectedBox db;
    for (int j = 0; j < 4; ++j) {
      db.points.emplace_back(
          static_cast<int>(std::lround(boxes[i].box[j * 2])),
          static_cast<int>(std::lround(boxes[i].box[j * 2 + 1])));
    }
    db.branch_index = boxes[i].branch_index;
    db.score = boxes[i].score;
    detected.push_back(std::move(db));
  }

  auto regions =
      engine->engine.MergeRegions(detected, parsed.merge, image_width,
                                  image_height);
  engine->box_buffer.clear();
  engine->box_buffer.reserve(regions.size());
  for (const auto& mb : regions) {
    PpOcrBoxInput item{};
    for (int j = 0; j < 4 && j < static_cast<int>(mb.points.size()); ++j) {
      item.box[j * 2] = static_cast<double>(mb.points[j].x);
      item.box[j * 2 + 1] = static_cast<double>(mb.points[j].y);
    }
    item.branch_index = 0;
    item.score = mb.score;
    engine->box_buffer.push_back(item);
  }
  engine->box_array.items = engine->box_buffer.data();
  engine->box_array.count = static_cast<int32_t>(engine->box_buffer.size());
  engine->box_array.image_width = image_width;
  engine->box_array.image_height = image_height;
  engine->box_array.error = nullptr;
  *out = engine->box_array;
  engine->last_error.clear();
  return 1;
}

PP_OCR_EXPORT int pp_ocr_recognize_box_bytes(PpOcrEngine* engine,
                                             const uint8_t* data, int length,
                                             const double box[8],
                                             const PpOcrOptions* options,
                                             PpOcrTextResult* out) {
  if (!engine) return 0;
  if (!out || !box) {
    engine->last_error = "Null out argument";
    return 0;
  }
  cv::Mat image;
  if (!DecodeInput(engine, data, length, &image)) return 0;
  if (!engine->engine.IsInitialized()) {
    engine->last_error = "Engine not initialized";
    return 0;
  }

  paddle_ocr::OcrOptions parsed;
  if (options) {
    if (!ParseOptions(engine, options, &parsed)) return 0;
  } else {
    paddle_ocr::FillDefaultOptions(&parsed);
  }

  std::vector<cv::Point> points;
  for (int j = 0; j < 4; ++j) {
    points.emplace_back(static_cast<int>(std::lround(box[j * 2])),
                        static_cast<int>(std::lround(box[j * 2 + 1])));
  }
  auto [text, conf] = engine->engine.RecognizeBox(image, points, parsed.rec);
  engine->single_text_buffer = std::move(text);
  engine->text_out.text = engine->single_text_buffer.c_str();
  engine->text_out.confidence = static_cast<double>(conf);
  *out = engine->text_out;
  engine->last_error.clear();
  return 1;
}

}  // extern "C"
