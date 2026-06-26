#ifndef PP_OCR_FFI_H_
#define PP_OCR_FFI_H_

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
  #define PP_OCR_EXPORT __declspec(dllexport)
#else
  #define PP_OCR_EXPORT __attribute__((visibility("default")))
#endif

#include <stdint.h>

/// Opaque handle to an OCR engine instance.
typedef struct PpOcrEngine PpOcrEngine;

/// A single OCR result: bounding box + text + confidence.
/// Box points are [x0,y0, x1,y1, x2,y2, x3,y3] (TL, TR, BR, BL).
typedef struct {
  double box[8];       // 4 points, each x,y
  const char* text;    // UTF-8 string (owned by engine, valid until next call)
  double confidence;   // 0.0 ~ 1.0
} PpOcrResultItem;

/// Array of OCR results returned from recognition.
typedef struct {
  PpOcrResultItem* items;  // pointer to array (owned by engine)
  int count;               // number of items
  const char* error;       // error message if any (nullptr on success)
} PpOcrResultArray;

/// Create a new OCR engine instance.
/// Returns nullptr on failure.
PP_OCR_EXPORT PpOcrEngine* pp_ocr_create(void);

/// Destroy an OCR engine instance.
/// Passing nullptr is safe.
PP_OCR_EXPORT void pp_ocr_destroy(PpOcrEngine* engine);

/// Initialize the OCR engine with model paths.
/// Returns 1 on success, 0 on failure.
/// On failure, call pp_ocr_get_last_error() for details.
PP_OCR_EXPORT int pp_ocr_initialize(PpOcrEngine* engine,
                                      const char* det_model_path,
                                      const char* rec_model_path,
                                      const char* dict_path);

/// Check if the engine is initialized.
/// Returns 1 if initialized, 0 otherwise.
PP_OCR_EXPORT int pp_ocr_is_initialized(PpOcrEngine* engine);

/// Get the last error message.
/// Returns nullptr if no error. The string is owned by the engine.
PP_OCR_EXPORT const char* pp_ocr_get_last_error(PpOcrEngine* engine);

/// Recognize text from an image file.
/// Returns a result array. The caller must NOT free the returned pointer.
/// The returned data is valid until the next call to this engine.
/// If an error occurs, result.error will be set.
PP_OCR_EXPORT PpOcrResultArray pp_ocr_recognize_file(
    PpOcrEngine* engine, const char* image_path);

/// Recognize text from image bytes (e.g. PNG/JPEG data).
/// Returns a result array. Same ownership rules as pp_ocr_recognize_file.
PP_OCR_EXPORT PpOcrResultArray pp_ocr_recognize_bytes(
    PpOcrEngine* engine, const uint8_t* data, int length);

/// Get the version string of the OCR library.
/// Returns a static string, e.g. "pp_ocr 0.2.0".
PP_OCR_EXPORT const char* pp_ocr_version(void);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // PP_OCR_FFI_H_
