#ifndef OCR_ENGINE_H_
#define OCR_ENGINE_H_

#include <string>
#include <vector>
#include <memory>
#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>

namespace paddle_ocr {

// OCR result for a single detected text region
struct OcrBoxResult {
  std::vector<cv::Point> box;  // 4 corner points
  std::string text;
  float confidence;
};

// Text detection using DB (Differentiable Binarization) model
class TextDetector {
 public:
  TextDetector();
  ~TextDetector();

  bool Initialize(const std::string& model_path);
  std::vector<std::vector<cv::Point>> Detect(const cv::Mat& image);

 private:
  Ort::Env env_;
  Ort::Session session_{nullptr};
  std::string input_name_ = "x";
  std::string output_name_ = "save_infer_model/scale_0.tmp_1";

  float det_db_thresh_ = 0.3f;
  float det_db_box_thresh_ = 0.5f;
  float det_db_unclip_ratio_ = 1.5f;
  int max_side_len_ = 960;

  cv::Mat Preprocess(const cv::Mat& image, float& ratio_h, float& ratio_w);
  std::vector<std::vector<cv::Point>> PostProcess(
      const float* output, int out_h, int out_w,
      float ratio_h, float ratio_w);
  std::vector<std::vector<cv::Point>> BoxesFromBitmap(
      const cv::Mat& pred, const cv::Mat& mask,
      float ratio_w, float ratio_h);
  void Unclip(const std::vector<cv::Point>& in_poly,
              std::vector<cv::Point>& out_poly, float unclip_ratio);
  float BoxScore(const cv::Mat& pred, const std::vector<cv::Point>& box);
};

// Text recognition using CRNN + CTC model
class TextRecognizer {
 public:
  TextRecognizer();
  ~TextRecognizer();

  bool Initialize(const std::string& model_path,
                  const std::string& dict_path);

  std::vector<std::pair<std::string, float>> Recognize(
      const cv::Mat& image,
      const std::vector<std::vector<cv::Point>>& boxes);

 private:
  Ort::Env env_;
  Ort::Session session_{nullptr};
  std::string input_name_ = "x";
  std::string output_name_ = "save_infer_model/scale_0.tmp_1";
  std::vector<std::string> dict_;
  int rec_img_h_ = 48;

  bool LoadDict(const std::string& dict_path);
  cv::Mat PreprocessCrop(const cv::Mat& image,
                         const std::vector<cv::Point>& box);
  std::pair<std::string, float> DecodeOutput(
      const float* data, int seq_len, int num_classes);
};

// Main OCR engine combining detection and recognition
class OcrEngine {
 public:
  OcrEngine();
  ~OcrEngine();

  bool Initialize(const std::string& det_model_path,
                  const std::string& rec_model_path,
                  const std::string& dict_path);

  std::vector<OcrBoxResult> Recognize(const cv::Mat& image);
  std::vector<OcrBoxResult> RecognizeFromBytes(
      const std::vector<uint8_t>& image_bytes);
  std::vector<OcrBoxResult> RecognizeFromFile(const std::string& image_path);

  bool IsInitialized() const { return initialized_; }
  const std::string& GetLastError() const { return last_error_; }

 private:
  TextDetector detector_;
  TextRecognizer recognizer_;
  bool initialized_ = false;
  std::string last_error_;
};

}  // namespace paddle_ocr

#endif  // OCR_ENGINE_H_
