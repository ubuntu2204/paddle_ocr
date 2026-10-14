#ifndef OCR_ENGINE_H_
#define OCR_ENGINE_H_

#include <string>
#include <vector>
#include <memory>
#include <onnxruntime_cxx_api.h>
#include <opencv2/opencv.hpp>

#include "ocr_options.h"
#include "ocr_preprocess.h"

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
  // （使用内置默认参数，保留给老调用方）
  std::vector<std::vector<cv::Point>> Detect(const cv::Mat& image);
  // 带参数检测：返回框 + DB 平均得分，供多分支合并与择优识别使用
  std::vector<DetectedBox> DetectScored(
      const cv::Mat& image, const DetectParams& params, int branch_index);

 private:
  Ort::Env env_;                 // ONNX Runtime 运行环境
  Ort::Session session_{nullptr};  // ONNX Runtime 推理会话
  std::string input_name_ = "x";  // 模型输入节点名称
  std::string output_name_ = "save_infer_model/scale_0.tmp_1";  // 模型输出节点名称

  // 内置默认参数（等价于 kPresetStandard）；实际推理一律走 DetectParams
  static DetectParams DefaultParams();

  // 图像预处理：缩放、归一化，返回处理后的 Mat 及缩放比例
  cv::Mat Preprocess(const cv::Mat& image, const DetectParams& params,
                     float& ratio_h, float& ratio_w);
  // 后处理：将模型输出转换为文本框列表
  std::vector<DetectedBox> PostProcess(
      const float* output, int out_h, int out_w,
      float ratio_h, float ratio_w, const DetectParams& params,
      int branch_index);
  // 从预测图（概率图）和二值化掩码中提取文本框
  std::vector<DetectedBox> BoxesFromBitmap(
      const cv::Mat& pred, const cv::Mat& mask,
      float ratio_w, float ratio_h, const DetectParams& params,
      int branch_index);
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

  // 带参数的批量识别（使用 params 里的输入尺寸与文本条二次预处理配置）
  std::vector<std::pair<std::string, float>> Recognize(
      const cv::Mat& image,
      const std::vector<std::vector<cv::Point>>& boxes,
      const RecognizeParams& params);

  // 识别单个区域。多分支择优识别需要对同一区域在多个图像变体上反复调用，
  // 因此单独暴露。返回（文本, 置信度）。
  std::pair<std::string, float> RecognizeRegion(
      const cv::Mat& image, const std::vector<cv::Point>& box,
      const RecognizeParams& params);

 private:
  Ort::Env env_;                 // ONNX Runtime 运行环境
  Ort::Session session_{nullptr};  // ONNX Runtime 推理会话
  std::string input_name_ = "x";  // 模型输入节点名称
  std::string output_name_ = "save_infer_model/scale_0.tmp_1";  // 模型输出节点名称
  std::vector<std::string> dict_;  // 识别字典（字符表）

  // 加载字典文件
  bool LoadDict(const std::string& dict_path);
  // 根据文本框从原图裁剪并预处理文本区域图像（含缩放与归一化）
  cv::Mat PreprocessCrop(const cv::Mat& image,
                         const std::vector<cv::Point>& box,
                         const RecognizeParams& params);
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

  // 设置全局默认选项。设置后，不传 options 的识别接口都会用它，
  // 宿主 App 可以「一次配置、处处生效」。
  void SetDefaultOptions(const OcrOptions& options);
  // 读取当前默认选项
  const OcrOptions& GetDefaultOptions() const { return default_options_; }
  // 清空默认选项，回到标准单分支行为
  void ClearDefaultOptions();

  // 对 cv::Mat 图像进行 OCR 识别（使用默认选项）
  std::vector<OcrBoxResult> Recognize(const cv::Mat& image);
  // 对 cv::Mat 图像进行 OCR 识别（显式选项）
  std::vector<OcrBoxResult> Recognize(const cv::Mat& image,
                                       const OcrOptions& options);
  // 对图像字节数据（如 PNG/JPEG）进行 OCR 识别
  std::vector<OcrBoxResult> RecognizeFromBytes(
      const std::vector<uint8_t>& image_bytes);
  std::vector<OcrBoxResult> RecognizeFromBytes(
      const std::vector<uint8_t>& image_bytes, const OcrOptions& options);
  // 对图像文件进行 OCR 识别
  std::vector<OcrBoxResult> RecognizeFromFile(const std::string& image_path);
  std::vector<OcrBoxResult> RecognizeFromFile(const std::string& image_path,
                                               const OcrOptions& options);

  // =========================================================================
  // 原子操作（供宿主 App 编排自定义识别管线用）
  //
  // 完整管线（Recognize*）固定使用内置预处理分支；宿主若想注入自己的
  // 预处理（超分、色彩域提取、去雾……），可以把图变体准备好后用下面
  // 四个原子步骤自行编排「多路检测 → 合并 → 择优识别」，与 C++ 内部
  // 管线共享同一套模型与几何后处理实现。
  // =========================================================================

  // 原子 1：对一张（已预处理的）图跑文本检测。框坐标在**输入图**坐标系。
  std::vector<DetectedBox> DetectOnImage(const cv::Mat& image,
                                         const DetectParams& params);
  // 原子 2：合并多路检测框（去重 + 同行拼接 + 外扩 + 排序）。
  // box 坐标必须在**同一张底图**坐标系（不同分支的框请先换算回去）。
  std::vector<MergedBox> MergeRegions(const std::vector<DetectedBox>& boxes,
                                      const MergeParams& params,
                                      int image_width, int image_height);
  // 原子 3：在指定图上识别一个文本区域。enhance_crops 择优语义与完整
  // 管线一致（0/1/2）。
  std::pair<std::string, float> RecognizeBox(
      const cv::Mat& image, const std::vector<cv::Point>& box,
      const RecognizeParams& params);
  // 原子 4：对一张图跑内置预处理（宿主把结果编码回字节流交给自己的
  // 流程，或者直接拿去 DetectOnImage）。
  cv::Mat ApplyImageTransform(const cv::Mat& image,
                              const PreprocessParams& params);
  // 多变体择优打分（与完整管线同款公式）。宿主编排时用它选最优候选。
  static double ScoreVariant(int32_t variant_select, const std::string& text,
                             float confidence, double long_side,
                             double short_side, bool raw_crop);

  // 解码图片字节为 cv::Mat；失败返回空 Mat。供上层与测试复用。
  static cv::Mat DecodeImageBytes(const std::vector<uint8_t>& image_bytes);

  // 把某一路预处理的产物单独拿出来（调试 / 宿主想自己看图时用）。
  // branch_flag 取 BranchFlag 中的单个位；未知位返回空 Mat。
  static cv::Mat RenderBranchPreview(const cv::Mat& image,
                                      uint32_t branch_flag,
                                      const OcrOptions& options);

  // 引擎是否已初始化
  bool IsInitialized() const { return initialized_; }
  // 获取最近一次错误信息
  const std::string& GetLastError() const { return last_error_; }

 private:
  // 单路检测分支的运行期产物
  struct BranchOutput {
    cv::Mat image;                     // 预处理后的图（识别时从这里裁剪）
    std::vector<DetectedBox> boxes;    // 框坐标已换算回原图坐标系
    double scale = 1.0;                // image 相对原图的缩放系数
    uint32_t flag = 0;                 // 对应的 BranchFlag
    std::string name;
    bool rec_source = true;            // 能否作为识别源图（二值化类分支不行）
  };

  // 把原图坐标系下的框换算到某一路分支图坐标系
  static std::vector<cv::Point> ScaleBoxToBranch(
      const std::vector<cv::Point>& box, double scale);

  // 完整多分支管道：预处理分支 → 检测 → 合并 → 同行拼接 → 择优识别
  std::vector<OcrBoxResult> RunPipeline(const cv::Mat& image,
                                         const OcrOptions& options);
  // 单分支兼容路径（等价于历史行为）
  std::vector<OcrBoxResult> RunSingleBranch(
      const cv::Mat& image, const OcrOptions& options);

  TextDetector detector_;      // 文本检测器
  TextRecognizer recognizer_;  // 文本识别器
  bool initialized_ = false;   // 初始化状态标志
  std::string last_error_;     // 最近一次错误信息
  OcrOptions default_options_; // 全局默认选项
  bool has_explicit_default_ = false;  // 宿主是否调过 SetDefaultOptions
};

}  // namespace paddle_ocr

#endif  // OCR_ENGINE_H_
