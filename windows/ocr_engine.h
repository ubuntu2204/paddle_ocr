#ifndef OCR_ENGINE_H_
#define OCR_ENGINE_H_

#include <string>
#include <vector>
#include <memory>
#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>

namespace paddle_ocr {

// 单个检测到的文本区域的 OCR 结果
struct OcrBoxResult {
  std::vector<cv::Point> box;  // 文本区域的 4 个角点
  std::string text;            // 识别出的文本内容
  float confidence;            // 识别置信度（0.0 ~ 1.0）
};

// 文本检测器：基于 DB（可微二值化）模型
class TextDetector {
 public:
  TextDetector();
  ~TextDetector();

  // 初始化检测器，加载 ONNX 检测模型
  bool Initialize(const std::string& model_path);
  // 对输入图像进行文本检测，返回检测到的文本区域框列表
  std::vector<std::vector<cv::Point>> Detect(const cv::Mat& image);

 private:
  Ort::Env env_;                 // ONNX Runtime 运行环境
  Ort::Session session_{nullptr};  // ONNX Runtime 推理会话
  std::string input_name_ = "x";  // 模型输入节点名称
  std::string output_name_ = "save_infer_model/scale_0.tmp_1";  // 模型输出节点名称

  float det_db_thresh_ = 0.3f;        // DB 二值化阈值
  float det_db_box_thresh_ = 0.5f;    // 文本框得分阈值
  float det_db_unclip_ratio_ = 1.5f;  // 文本框扩展比例
  int max_side_len_ = 960;            // 输入图像最大边长（超过则缩放）

  // 图像预处理：缩放、归一化，返回处理后的 Mat 及缩放比例
  cv::Mat Preprocess(const cv::Mat& image, float& ratio_h, float& ratio_w);
  // 后处理：将模型输出转换为文本框列表
  std::vector<std::vector<cv::Point>> PostProcess(
      const float* output, int out_h, int out_w,
      float ratio_h, float ratio_w);
  // 从预测图（概率图）和二值化掩码中提取文本框
  std::vector<std::vector<cv::Point>> BoxesFromBitmap(
      const cv::Mat& pred, const cv::Mat& mask,
      float ratio_w, float ratio_h);
  // 对多边形进行扩展（Unclip），扩大文本框范围
  void Unclip(const std::vector<cv::Point>& in_poly,
              std::vector<cv::Point>& out_poly, float unclip_ratio);
  // 计算文本框在预测图中的平均得分
  float BoxScore(const cv::Mat& pred, const std::vector<cv::Point>& box);
};

// 文本识别器：基于 CRNN + CTC 模型
class TextRecognizer {
 public:
  TextRecognizer();
  ~TextRecognizer();

  // 初始化识别器，加载 ONNX 识别模型和字典文件
  bool Initialize(const std::string& model_path,
                  const std::string& dict_path);

  // 对图像中检测到的各文本框进行识别，返回（文本, 置信度）列表
  std::vector<std::pair<std::string, float>> Recognize(
      const cv::Mat& image,
      const std::vector<std::vector<cv::Point>>& boxes);

 private:
  Ort::Env env_;                 // ONNX Runtime 运行环境
  Ort::Session session_{nullptr};  // ONNX Runtime 推理会话
  std::string input_name_ = "x";  // 模型输入节点名称
  std::string output_name_ = "save_infer_model/scale_0.tmp_1";  // 模型输出节点名称
  std::vector<std::string> dict_;  // 识别字典（字符表）
  int rec_img_h_ = 48;             // 识别模型输入图像高度（固定）

  // 加载字典文件
  bool LoadDict(const std::string& dict_path);
  // 根据文本框从原图裁剪并对齐文本区域图像
  cv::Mat PreprocessCrop(const cv::Mat& image,
                         const std::vector<cv::Point>& box);
  // 对模型输出进行 CTC 解码，返回（文本, 平均置信度）
  std::pair<std::string, float> DecodeOutput(
      const float* data, int seq_len, int num_classes);
};

// OCR 引擎：组合文本检测器和文本识别器，提供完整的 OCR 流程
class OcrEngine {
 public:
  OcrEngine();
  ~OcrEngine();

  // 初始化引擎，加载检测模型、识别模型和字典
  bool Initialize(const std::string& det_model_path,
                  const std::string& rec_model_path,
                  const std::string& dict_path);

  // 对 cv::Mat 图像进行 OCR 识别
  std::vector<OcrBoxResult> Recognize(const cv::Mat& image);
  // 对图像字节数据（如 PNG/JPEG）进行 OCR 识别
  std::vector<OcrBoxResult> RecognizeFromBytes(
      const std::vector<uint8_t>& image_bytes);
  // 对图像文件进行 OCR 识别
  std::vector<OcrBoxResult> RecognizeFromFile(const std::string& image_path);

  // 引擎是否已初始化
  bool IsInitialized() const { return initialized_; }
  // 获取最近一次错误信息
  const std::string& GetLastError() const { return last_error_; }

 private:
  TextDetector detector_;      // 文本检测器
  TextRecognizer recognizer_;  // 文本识别器
  bool initialized_ = false;   // 初始化状态标志
  std::string last_error_;     // 最近一次错误信息
};

}  // namespace paddle_ocr

#endif  // OCR_ENGINE_H_
