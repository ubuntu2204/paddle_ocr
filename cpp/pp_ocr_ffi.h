#ifndef PP_OCR_FFI_H_
#define PP_OCR_FFI_H_

#ifdef __cplusplus
extern "C" {
#endif

// 跨平台导出宏定义
#if defined(_WIN32) || defined(__CYGWIN__)
  #define PP_OCR_EXPORT __declspec(dllexport)
#else
  #define PP_OCR_EXPORT __attribute__((visibility("default")))
#endif

#include <stdint.h>

/// OCR 引擎实例的不透明句柄
typedef struct PpOcrEngine PpOcrEngine;

/// 单个 OCR 结果项：边界框 + 文本 + 置信度
/// 边界框点为 [x0,y0, x1,y1, x2,y2, x3,y3]（依次为左上、右上、右下、左下）
typedef struct {
  double box[8];       // 4 个角点，每个点包含 x, y 坐标
  const char* text;    // UTF-8 编码的文本字符串（由引擎持有，有效期至下一次调用）
  double confidence;   // 置信度（0.0 ~ 1.0）
} PpOcrResultItem;

/// 识别返回的 OCR 结果数组
typedef struct {
  PpOcrResultItem* items;  // 指向结果数组（由引擎持有）
  int count;               // 结果项数量
  const char* error;       // 错误信息（成功时为 nullptr）
} PpOcrResultArray;

/// 创建新的 OCR 引擎实例
/// 失败时返回 nullptr
PP_OCR_EXPORT PpOcrEngine* pp_ocr_create(void);

/// 销毁 OCR 引擎实例
/// 传入 nullptr 是安全的
PP_OCR_EXPORT void pp_ocr_destroy(PpOcrEngine* engine);

/// 使用模型路径初始化 OCR 引擎
/// 成功返回 1，失败返回 0
/// 失败时可调用 pp_ocr_get_last_error() 获取详细信息
PP_OCR_EXPORT int pp_ocr_initialize(PpOcrEngine* engine,
                                      const char* det_model_path,
                                      const char* rec_model_path,
                                      const char* dict_path);

/// 检查引擎是否已初始化
/// 已初始化返回 1，否则返回 0
PP_OCR_EXPORT int pp_ocr_is_initialized(PpOcrEngine* engine);

/// 获取最近一次错误信息
/// 无错误时返回 nullptr。字符串由引擎持有
PP_OCR_EXPORT const char* pp_ocr_get_last_error(PpOcrEngine* engine);

/// 从图像文件识别文本
/// 返回结果数组。调用方不得释放返回的指针
/// 返回的数据在下一次调用该引擎前有效
/// 若发生错误，result.error 将被设置
PP_OCR_EXPORT PpOcrResultArray pp_ocr_recognize_file(
    PpOcrEngine* engine, const char* image_path);

/// 从图像字节数据（如 PNG/JPEG 数据）识别文本
/// 返回结果数组。所有权规则与 pp_ocr_recognize_file 相同
PP_OCR_EXPORT PpOcrResultArray pp_ocr_recognize_bytes(
    PpOcrEngine* engine, const uint8_t* data, int length);

/// 获取 OCR 库的版本字符串
/// 返回静态字符串，例如 "pp_ocr 0.2.0"
PP_OCR_EXPORT const char* pp_ocr_version(void);

// ===========================================================================
// 预处理与多分支识别选项
//
// 背景：单路检测在「白字压复杂背景」（监控叠加字幕、路牌、低对比扫描件）
// 上会把一行字碎裂成几段并丢头丢尾。下面的选项可以把图先做二值化/对比度
// 增强/放大，多路并行检测后合并去重，再对同一区域在多张图上择优识别。
//
// ABI 约定：
//   * PpOcrOptions 是定长平铺结构，可直接 memcpy，字段类型均为定长；
//   * 调用方必须先调 pp_ocr_options_init() 或 pp_ocr_options_preset() 填
//     默认值，再改自己关心的字段；
//   * struct_size 必须等于 pp_ocr_options_size() 的返回值，否则接口会
//     直接报错而不是静默读乱码。
// ===========================================================================

/// 二值化模式（与 C++ 侧 BinarizeMode 取值一致，一经发布不得重排）
enum {
  PP_OCR_BINARIZE_NONE = 0,
  PP_OCR_BINARIZE_OTSU_INV = 1,
  PP_OCR_BINARIZE_OTSU_PLAIN = 2,
  PP_OCR_BINARIZE_ADAPTIVE_GAUSS_INV = 3,
  PP_OCR_BINARIZE_ADAPTIVE_MEAN_INV = 4,
  PP_OCR_BINARIZE_AUTO = 5
};

/// 增强模式（与 C++ 侧 EnhanceMode 取值一致）
enum {
  PP_OCR_ENHANCE_NONE = 0,
  PP_OCR_ENHANCE_CLAHE = 1,
  PP_OCR_ENHANCE_SHARPEN = 2,
  PP_OCR_ENHANCE_CLAHE_SHARPEN = 3,
  PP_OCR_ENHANCE_DENOISE = 4,
  PP_OCR_ENHANCE_DENOISE_CLAHE = 5,
  PP_OCR_ENHANCE_WHITE_TEXT = 6
};

/// 通道处理（与 C++ 侧 ColorMode 取值一致）
enum {
  PP_OCR_COLOR_KEEP = 0,
  PP_OCR_COLOR_GRAY3 = 1
};

/// 检测分支位标志，可按位或组合
enum {
  PP_OCR_BRANCH_RAW = 1 << 0,
  PP_OCR_BRANCH_ADAPTIVE_BIN = 1 << 1,
  PP_OCR_BRANCH_OTSU_BIN = 1 << 2,
  PP_OCR_BRANCH_CLAHE = 1 << 3,
  PP_OCR_BRANCH_UPSCALE2X = 1 << 4,
  PP_OCR_BRANCH_SENSITIVE = 1 << 5,
  PP_OCR_BRANCH_SHARPEN = 1 << 6,
  PP_OCR_BRANCH_DENOISE_CLAHE = 1 << 7,
  PP_OCR_BRANCH_WHITE_TEXT = 1 << 8,
  /// 3 倍放大：2x 对 20px 级小字仍会偶发繁简误读（篮→籃），3x 实测完全正确。
  PP_OCR_BRANCH_UPSCALE3X = 1 << 9,
  /// 单路（等价于历史行为）
  PP_OCR_BRANCH_MONO = PP_OCR_BRANCH_RAW,
  /// 全部开启，最慢最全
  PP_OCR_BRANCH_ALL = PP_OCR_BRANCH_RAW | PP_OCR_BRANCH_ADAPTIVE_BIN |
                      PP_OCR_BRANCH_OTSU_BIN | PP_OCR_BRANCH_CLAHE |
                      PP_OCR_BRANCH_UPSCALE2X | PP_OCR_BRANCH_SENSITIVE |
                      PP_OCR_BRANCH_SHARPEN | PP_OCR_BRANCH_DENOISE_CLAHE |
                      PP_OCR_BRANCH_WHITE_TEXT | PP_OCR_BRANCH_UPSCALE3X,
  /// 监控画面推荐组合
  PP_OCR_BRANCH_SURVEILLANCE = PP_OCR_BRANCH_RAW |
                               PP_OCR_BRANCH_ADAPTIVE_BIN |
                               PP_OCR_BRANCH_CLAHE | PP_OCR_BRANCH_UPSCALE2X |
                               PP_OCR_BRANCH_UPSCALE3X |
                               PP_OCR_BRANCH_SENSITIVE |
                               PP_OCR_BRANCH_DENOISE_CLAHE |
                               PP_OCR_BRANCH_WHITE_TEXT
};

/// 预设编号（standard/document/lowContrast/smallText 已删除，编号不重排）
enum {
  PP_OCR_PRESET_SURVEILLANCE = 3,
  PP_OCR_PRESET_MAX_RECALL = 5
};

/// 识别选项。字段与 paddle_ocr::OcrOptions 一一对应，但平铺展开以便宿主绑定。
typedef struct {
  /// 必须等于 pp_ocr_options_size()，由 init/preset 自动填写
  int32_t struct_size;

  // ---- 文本检测（DB）----
  double det_thresh;              ///< 概率图二值化阈值，默认 0.30
  double det_box_thresh;          ///< 文本框得分阈值，默认 0.50
  double det_unclip_ratio;        ///< 文本框外扩比例，默认 1.5
  int32_t det_max_side_len;       ///< 输入最长边，默认 960
  int32_t det_min_box_size;       ///< 最小短边，默认 3
  int32_t det_min_box_area;       ///< 最小框面积，0 = 不限制

  // ---- 文本识别（CRNN + CTC）----
  int32_t rec_img_height;         ///< 识别输入高度，默认 48
  int32_t rec_max_img_width;      ///< 识别输入最大宽度，默认 2048
  /// 文本条二次预处理：0 = 从不，1 = 总是，2 = 两种都跑并择优
  int32_t rec_enhance_crops;
  /// 同一区域在哪些图上试识别：0 = 只试检出它的分支，1 = 试所有分支
  int32_t rec_try_all_branches;
  double rec_min_confidence;      ///< 低于此置信度的结果丢弃，0 = 全留
  /// 多变体择优策略：0=置信度 1=长度×置信度 2=长度 3=几何契合 4=3+原像素优先
  int32_t rec_variant_select;

  // ---- 文本条二次预处理的参数（仅 rec_enhance_crops != 0 时生效）----
  int32_t crop_binarize_mode;
  int32_t crop_adaptive_block_size;  ///< 0 = 按文本条高度自动取值
  double crop_adaptive_c;
  int32_t crop_enhance_mode;
  double crop_clahe_clip_limit;
  int32_t crop_clahe_grid_size;
  double crop_sharpen_amount;
  int32_t crop_morph_close_kernel;
  int32_t crop_morph_open_kernel;
  int32_t crop_color_mode;

  // ---- 多分支与区域合并 ----
  uint32_t branches;                  ///< PP_OCR_BRANCH_* 按位或
  double merge_iou_threshold;         ///< IoU 超过它视为同一区域
  double merge_contain_threshold;     ///< 包含度超过它视为重复
  int32_t stitch_same_line;           ///< 是否拼接同行碎框：0/1
  double line_min_y_overlap;
  double line_max_gap_ratio;
  double line_max_height_diff_ratio;
  double line_max_width_ratio;
  double region_pad_ratio;            ///< 区域外扩比例（相对字高）
  /// 识别窗沿行方向向两侧外扩的比例（相对字高）。把检测框紧邻的漏检字
  /// （孤立单横「一」等）纳入 CRNN 识别窗。0 = 不延伸。
  double region_pad_long_ratio;
  int32_t max_regions;                ///< 结果数上限，0 = 不限制
  int32_t sort_order;                 ///< 0=阅读顺序 1=面积降序 2=不排序

  // ---- 感兴趣区域：只识别矩形内的文字，全 0 = 整图 ----
  int32_t roi_x;
  int32_t roi_y;
  int32_t roi_w;
  int32_t roi_h;

  // ---- 诊断 ----
  int32_t verbose_stats;              ///< 1 = 把每路分支检出数打到日志
} PpOcrOptions;

/// 返回 sizeof(PpOcrOptions)，供调用方核对结构体版本
PP_OCR_EXPORT int32_t pp_ocr_options_size(void);

/// 用「标准」默认值填充（单分支、无预处理）
PP_OCR_EXPORT void pp_ocr_options_init(PpOcrOptions* out);

/// 用预设填充。preset_id 取 PP_OCR_PRESET_*；out 为 null 返回 0
PP_OCR_EXPORT int pp_ocr_options_preset(int32_t preset_id, PpOcrOptions* out);

/// 返回预设的可读名称（静态字符串，用于日志）
PP_OCR_EXPORT const char* pp_ocr_preset_name(int32_t preset_id);

/// 就地修正非法参数，返回被修正的字段个数（0 = 全部合法）
PP_OCR_EXPORT int32_t pp_ocr_options_sanitize(PpOcrOptions* options);

/// 设置引擎级默认选项；此后调不带 options 的识别接口都会用它。
/// 成功返回 1，失败返回 0（可用 pp_ocr_get_last_error 取原因）
PP_OCR_EXPORT int pp_ocr_set_default_options(PpOcrEngine* engine,
                                              const PpOcrOptions* options);

/// 清除引擎级默认选项，回到标准单分支行为
PP_OCR_EXPORT void pp_ocr_clear_default_options(PpOcrEngine* engine);

/// 带选项从文件识别。options 为 null 时等价于 pp_ocr_recognize_file
PP_OCR_EXPORT PpOcrResultArray pp_ocr_recognize_file_ex(
    PpOcrEngine* engine, const char* image_path, const PpOcrOptions* options);

/// 带选项从字节识别。options 为 null 时等价于 pp_ocr_recognize_bytes
PP_OCR_EXPORT PpOcrResultArray pp_ocr_recognize_bytes_ex(
    PpOcrEngine* engine, const uint8_t* data, int length,
    const PpOcrOptions* options);

// ===========================================================================
// 原子操作（宿主编排自定义识别管线的插件接口）
//
// 完整识别接口（pp_ocr_recognize_*_ex）固定使用内置预处理分支；宿主若
// 想注入自己的预处理（超分、色彩域提取、去雾……），用下面四个原子步骤
// 自行编排「预处理 → 检测 → 合并 → 识别」，与 C++ 内部管线共享同一套
// 模型与几何后处理：
//
//   1. pp_ocr_preprocess_bytes：对整图跑一次内置预处理变换；
//   2. pp_ocr_detect_bytes：对一张（已预处理的）图跑文本检测；
//   3. pp_ocr_merge_boxes：把多路检测框合并成「识别窗」列表
//      （去重 + 同行拼接 + 成员基底外扩，输出的框可直接裁剪识别）；
//   4. pp_ocr_recognize_box_bytes：在指定图上识别一个文本窗。
//
// 坐标约定：所有框都在「传给本次调用的那张图」的坐标系；宿主对放大
// 变体检出的框，需自行按缩放系数换算回底图坐标再交给 merge。
// ===========================================================================

/// 预处理变换参数（对应 C++ PreprocessParams 的平铺形式）。
typedef struct {
  int32_t struct_size;              ///< 必须等于 transform_params_size()
  int32_t scale_percent;            ///< 100 = 原尺寸，200 = 放大一倍
  int32_t binarize_mode;            ///< PP_OCR_BINARIZE_*
  int32_t adaptive_block_size;      ///< 0 = 按图尺寸自动取
  double adaptive_c;
  int32_t enhance_mode;             ///< PP_OCR_ENHANCE_*
  double clahe_clip_limit;
  int32_t clahe_grid_size;
  double sharpen_amount;
  int32_t morph_open_kernel;        ///< 0 = 关闭
  int32_t morph_close_kernel;       ///< 0 = 关闭
  int32_t color_mode;               ///< PP_OCR_COLOR_*
} PpOcrTransformParams;

/// 返回 sizeof(PpOcrTransformParams)。
PP_OCR_EXPORT int32_t pp_ocr_transform_params_size(void);

/// 用默认值填充变换参数（不缩放、不增强、不二值化）。
PP_OCR_EXPORT void pp_ocr_transform_params_init(PpOcrTransformParams* out);

/// 引擎持有的图像缓冲区（下一次调用同引擎的任何接口前有效）。
typedef struct {
  const uint8_t* data;  ///< PNG 编码字节
  int32_t size;
} PpOcrImageBuffer;

/// 检测框（4 角点 + 来源信息）。
typedef struct {
  double box[8];
  int32_t branch_index;
  double score;
} PpOcrBoxInput;

/// 检测框数组（引擎持有，下一次调用前有效）。
typedef struct {
  PpOcrBoxInput* items;
  int32_t count;
  int32_t image_width;  ///< 本次检测/合并输入图的宽（框坐标以它为基准）
  int32_t image_height;
  const char* error;  ///< 成功时为 nullptr
} PpOcrBoxArray;

/// 单窗识别结果（引擎持有，下一次调用前有效）。
typedef struct {
  const char* text;
  double confidence;
} PpOcrTextResult;

/// 原子 1：对图像字节跑一次内置预处理变换，输出 PNG 编码字节。
/// 成功返回 1；失败返回 0（可用 pp_ocr_get_last_error 取原因）。
PP_OCR_EXPORT int pp_ocr_preprocess_bytes(PpOcrEngine* engine,
                                          const uint8_t* data, int length,
                                          const PpOcrTransformParams* params,
                                          PpOcrImageBuffer* out);

/// 原子 2：对图像字节跑文本检测。options 的 det.* 部分生效；
/// 框坐标在输入图坐标系。失败返回 0。
PP_OCR_EXPORT int pp_ocr_detect_bytes(PpOcrEngine* engine,
                                      const uint8_t* data, int length,
                                      const PpOcrOptions* options,
                                      PpOcrBoxArray* out);

/// 原子 3：合并多路检测框（去重 + 同行拼接 + 识别窗构造）。
/// options 的 merge.* 部分生效；输出的框即为「识别窗」。
/// 框坐标必须在同一张底图坐标系（image_width/height 为底图尺寸）。
/// 失败返回 0。
PP_OCR_EXPORT int pp_ocr_merge_boxes(PpOcrEngine* engine,
                                     const PpOcrBoxInput* boxes, int count,
                                     const PpOcrOptions* options,
                                     int image_width, int image_height,
                                     PpOcrBoxArray* out);

/// 原子 4：在图像字节上识别一个文本窗。options 的 rec.* 部分生效
/// （含 enhance_crops 择优）。box 为 4 角点 [x0,y0,...,x3,y3]。
/// 失败返回 0。
PP_OCR_EXPORT int pp_ocr_recognize_box_bytes(PpOcrEngine* engine,
                                             const uint8_t* data, int length,
                                             const double box[8],
                                             const PpOcrOptions* options,
                                             PpOcrTextResult* out);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // PP_OCR_FFI_H_
