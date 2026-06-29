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

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // PP_OCR_FFI_H_
