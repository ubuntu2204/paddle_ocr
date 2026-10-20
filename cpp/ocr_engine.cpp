#include "ocr_engine.h"
#include "debug_utils.h"
#include "ocr_thread_pool.h"
#include "paddle_ocr_timing.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <numeric>
#include <sstream>
#ifdef _WIN32
#include <codecvt>
#include <locale>
#endif
// SCREENSHOT_CMAKE_PATCHED_V2: C API headers for cvFindContours (STL ABI workaround on Windows)
#include "opencv2/core/core_c.h"
#include "opencv2/imgproc/imgproc_c.h"

#ifndef _WIN32
#include <cerrno>
#endif

namespace paddle_ocr {

// SCREENSHOT_CMAKE_PATCHED: copy std::vector<cv::Point> into a cv::Mat
// (1, N, CV_32SC2) so it can be passed to OpenCV via InputArray without
// crossing the DLL boundary with a Debug-layout vector. cv::Mat's layout
// is independent of _HAS_ITERATOR_DEBUGGING.
static cv::Mat _PatchedPointsToMat(const std::vector<cv::Point>& pts) {
  cv::Mat m(1, static_cast<int>(pts.size()), CV_32SC2);
  if (!pts.empty()) {
    std::memcpy(m.data, pts.data(), pts.size() * sizeof(cv::Point));
  }
  return m;
}

// ============================================================================
// 融合的张量构建（性能关键路径）
//
// 原实现是「convertTo + split + 3 次通道算术 + merge + split + 3 次
// memcpy」≈ 10 趟全图扫描；检测输入动辄 3000×1500×3，光搬运就要几百毫秒。
// 这里一次遍历直接把 8UC3 BGR 像素写进 NCHW float 缓冲区，逐位复现原
// 数值顺序（同样的 float 中间量与运算次序），因此模型输出不变。
//
// det 归一化：(v/255 - mean)/std，均值方差按 ImageNet（B/G/R 顺序见下）；
// rec 归一化：(v/255 - 0.5)/0.5。
// 通道序：输出通道 0=B、1=G、2=R（与旧实现 split/merge 的结果一致）。
// ============================================================================
static void FillNchwTensor(const cv::Mat& bgr, float* out, int h, int w,
                           bool rec_style) {
  const size_t plane = static_cast<size_t>(h) * w;
  // ImageNet 均值/标准差，通道序 B,G,R。
  // 注意：B 通道的 std 历史上写的是 0.225（ImageNet 标准值是 0.229）——
  // 这是既有行为，监控字幕预设的识别质量是按这组数值调出来的，**刻意
  // 保留原样**；要改的话必须单独做 A/B 实验，不能混在性能重构里。
  const float mean[3] = {0.406f, 0.456f, 0.485f};
  const float std_[3] = {0.225f, 0.224f, 0.229f};
  cv::parallel_for_(cv::Range(0, h), [&](const cv::Range& r) {
    for (int y = r.start; y < r.end; ++y) {
      const cv::Vec3b* row = bgr.ptr<cv::Vec3b>(y);
      float* p0 = out + static_cast<size_t>(y) * w;  // B
      float* p1 = p0 + plane;                        // G
      float* p2 = p1 + plane;                        // R
      for (int x = 0; x < w; ++x) {
        const cv::Vec3b& px = row[x];
        // 与 convertTo(CV_32FC3, 1/255) 相同的取整路径：double 乘法后
        // 窄化到 float，再按 float 做减除。
        const float vb =
            static_cast<float>(static_cast<double>(px[0]) * (1.0 / 255.0));
        const float vg =
            static_cast<float>(static_cast<double>(px[1]) * (1.0 / 255.0));
        const float vr =
            static_cast<float>(static_cast<double>(px[2]) * (1.0 / 255.0));
        if (rec_style) {
          p0[x] = (vb - 0.5f) / 0.5f;
          p1[x] = (vg - 0.5f) / 0.5f;
          p2[x] = (vr - 0.5f) / 0.5f;
        } else {
          p0[x] = (vb - mean[0]) / std_[0];
          p1[x] = (vg - mean[1]) / std_[1];
          p2[x] = (vr - mean[2]) / std_[2];
        }
      }
    }
  });
}

// 两组 float 张量内容是否逐字节相同（分组推理前的确认）。
static bool TensorsBitwiseEqual(const std::vector<float>& a,
                                const std::vector<float>& b) {
  if (a.size() != b.size()) return false;
  if (a.empty()) return true;
  return std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

// ============================================================================
// 工具函数：验证并清理 UTF-8 字符串
// 剔除任何无效的字节序列，防止 Dart 端抛出 FormatException
// ============================================================================
static std::string SanitizeUtf8(const std::string& input) {
  std::string result;
  result.reserve(input.size());
  size_t i = 0;
  while (i < input.size()) {
    unsigned char c = static_cast<unsigned char>(input[i]);
    int seq_len = 0;
    if (c <= 0x7F) {
      seq_len = 1;           // ASCII 单字节
    } else if ((c & 0xE0) == 0xC0) {
      seq_len = 2;           // 2 字节序列
    } else if ((c & 0xF0) == 0xE0) {
      seq_len = 3;           // 3 字节序列
    } else if ((c & 0xF8) == 0xF0) {
      seq_len = 4;           // 4 字节序列
    } else {
      // 无效的起始字节，跳过
      i++;
      continue;
    }
    if (i + seq_len > input.size()) {
      // 序列被截断，跳过剩余字节
      break;
    }
    bool valid = true;
    for (int j = 1; j < seq_len; ++j) {
      if ((static_cast<unsigned char>(input[i + j]) & 0xC0) != 0x80) {
        valid = false;       // 续延字节不符合要求
        break;
      }
    }
    if (valid) {
      result.append(input, i, seq_len);
    }
    i += seq_len;
  }
  return result;
}

#ifdef _WIN32
// ============================================================================
// 工具函数：将 std::string (UTF-8) 转换为 std::wstring (宽字符，用于 Windows API/ORT)
// Linux 上 ORT 与 std::ifstream 直接接受 UTF-8 char* 路径，无需此转换。
// ============================================================================
static std::wstring ToWString(const std::string& s) {
  if (s.empty()) return L"";
  int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring ws(len - 1, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
  return ws;
}
#endif

// ============================================================================
// 工具函数：GetRotatedCropImage - 通过四点多边形裁剪文本区域
// 将倾斜的文本区域透视变换为水平矩形
// ============================================================================
static cv::Mat GetRotatedCropImage(const cv::Mat& image,
                                   const std::vector<cv::Point>& pts) {
  if (pts.size() != 4) return cv::Mat();

  // 将点重排为标准顺序：左上(TL)、右上(TR)、右下(BR)、左下(BL)。
  // cv::minAreaRect().points() 返回的点顺序不确定，可能导致宽高颠倒。
  // 策略：先按 y 排序得到上2点和下2点，再在每对中按 x 排序。
  std::vector<cv::Point> p = pts;
  std::sort(p.begin(), p.end(),
            [](const cv::Point& a, const cv::Point& b) {
              if (a.y != b.y) return a.y < b.y;
              return a.x < b.x;
            });
  // p[0], p[1] 是上2点（y最小）；p[2], p[3] 是下2点。
  // 上2点中：x小=左上(TL)，x大=右上(TR)
  // 下2点中：x小=左下(BL)，x大=右下(BR)
  cv::Point tl, tr, bl, br;
  if (p[0].x <= p[1].x) { tl = p[0]; tr = p[1]; }
  else                  { tl = p[1]; tr = p[0]; }
  if (p[2].x <= p[3].x) { bl = p[2]; br = p[3]; }
  else                  { bl = p[3]; br = p[2]; }
  std::vector<cv::Point> ordered = {tl, tr, br, bl};

  // 从有序点计算宽度(TL->TR)和高度(TL->BL)
  float w1 = static_cast<float>(std::sqrt(
      std::pow(ordered[0].x - ordered[1].x, 2.0) +
      std::pow(ordered[0].y - ordered[1].y, 2.0)));  // TL->TR
  float w2 = static_cast<float>(std::sqrt(
      std::pow(ordered[2].x - ordered[3].x, 2.0) +
      std::pow(ordered[2].y - ordered[3].y, 2.0)));  // BR->BL
  int crop_w = static_cast<int>(std::max(w1, w2));

  float h1 = static_cast<float>(std::sqrt(
      std::pow(ordered[0].x - ordered[3].x, 2.0) +
      std::pow(ordered[0].y - ordered[3].y, 2.0)));  // TL->BL
  float h2 = static_cast<float>(std::sqrt(
      std::pow(ordered[1].x - ordered[2].x, 2.0) +
      std::pow(ordered[1].y - ordered[2].y, 2.0)));  // TR->BR
  int crop_h = static_cast<int>(std::max(h1, h2));

  if (crop_w <= 0 || crop_h <= 0) return cv::Mat();

  // 源点（原图中的四点）
  std::vector<cv::Point2f> src_pts = {
      cv::Point2f(static_cast<float>(ordered[0].x), static_cast<float>(ordered[0].y)),
      cv::Point2f(static_cast<float>(ordered[1].x), static_cast<float>(ordered[1].y)),
      cv::Point2f(static_cast<float>(ordered[2].x), static_cast<float>(ordered[2].y)),
      cv::Point2f(static_cast<float>(ordered[3].x), static_cast<float>(ordered[3].y))};

  // 目标点（裁剪后矩形的四角）
  std::vector<cv::Point2f> dst_pts = {
      cv::Point2f(0, 0),
      cv::Point2f(static_cast<float>(crop_w - 1), 0),
      cv::Point2f(static_cast<float>(crop_w - 1),
                  static_cast<float>(crop_h - 1)),
      cv::Point2f(0, static_cast<float>(crop_h - 1))};

  // 透视变换矩阵
  cv::Mat M = cv::getPerspectiveTransform(src_pts.data(), dst_pts.data()); // SCREENSHOT_CMAKE_PATCHED: pointer overload
  cv::Mat crop_img;
  cv::warpPerspective(image, crop_img, M, cv::Size(crop_w, crop_h),
                      cv::BORDER_REPLICATE);
  return crop_img;
}

// ============================================================================
// 文本检测器 TextDetector 实现
// 基于 DB (Differentiable Binarization) 模型的文字检测
// ============================================================================

TextDetector::TextDetector() : env_(ORT_LOGGING_LEVEL_WARNING, "OcrDet") {}
TextDetector::~TextDetector() = default;

// 内置默认检测参数：与历史硬编码值一致，保证不传选项时行为不变。
DetectParams TextDetector::DefaultParams() {
  DetectParams p;
  FillDefaultDetect(&p);
  return p;
}

// 初始化检测模型
bool TextDetector::Initialize(const std::string& model_path) {
  Ort::SessionOptions session_options;
  // 推理线程数按 CPU 核数取（留一半给并行阶段），见 SessionThreadCount。
  session_options.SetIntraOpNumThreads(SessionThreadCount());
  session_options.SetGraphOptimizationLevel(
      GraphOptimizationLevel::ORT_ENABLE_ALL);

#ifdef _WIN32
  std::wstring wide_path = ToWString(model_path);
  // 让 Ort::Exception 向上传播，以便 OcrEngine 捕获错误信息
  session_ = Ort::Session(env_, wide_path.c_str(), session_options);
#else
  // Linux / macOS：ORT 接受 UTF-8 char* 路径
  session_ = Ort::Session(env_, model_path.c_str(), session_options);
#endif
  Ort::AllocatorWithDefaultOptions alloc;
  auto in_name = session_.GetInputNameAllocated(0, alloc);
  auto out_name = session_.GetOutputNameAllocated(0, alloc);
  input_name_ = in_name.get();
  output_name_ = out_name.get();
  return true;
}

// 检测图片中的文本区域（兼容旧签名，丢弃得分）
std::vector<std::vector<cv::Point>> TextDetector::Detect(const cv::Mat& image) {
  std::vector<std::vector<cv::Point>> boxes;
  for (const auto& db : DetectScored(image, DefaultParams(), 0)) {
    boxes.push_back(db.points);
  }
  return boxes;
}

// 带参数的文本检测：返回框 + DB 平均得分
std::vector<DetectedBox> TextDetector::DetectScored(
    const cv::Mat& image, const DetectParams& params, int branch_index) {
  std::vector<DetectedBox> no_boxes;
  if (!session_ || image.empty()) return no_boxes;

  PaddleOcrTimer t_total("det.total");
  std::vector<float> input_data;
  int input_h = 0, input_w = 0;
  float ratio_h = 1.0f, ratio_w = 1.0f;
  {
    PaddleOcrTimer t("det.preprocess");
    if (!BuildDetectInput(image, params, input_data, input_h, input_w, ratio_h,
                          ratio_w)) {
      return no_boxes;
    }
  }

  OCR_LOG("Detect[b%d]: input image %dx%d -> resized %dx%d",
          branch_index, image.cols, image.rows, input_w, input_h);
  OCR_LOG("Detect[b%d]: ratio_h=%.4f ratio_w=%.4f thresh=%.3f box_thresh=%.3f",
          branch_index, ratio_h, ratio_w, static_cast<float>(params.thresh),
          static_cast<float>(params.box_thresh));

  std::vector<float> out_data;
  int out_h = 0, out_w = 0;
  {
    PaddleOcrTimer t_infer("det.infer");
    if (!RunDetectInference(input_data.data(), input_h, input_w, out_data, out_h,
                            out_w)) {
      return no_boxes;
    }
  }

  PaddleOcrTimer t_post("det.postprocess");
  auto boxes = PostProcess(out_data.data(), out_h, out_w, ratio_h, ratio_w,
                           params, branch_index);
  t_post.Stop();
  return boxes;
}

// 构建检测模型输入：缩放 + 32 对齐填充 + 归一化，直接写 NCHW float 缓冲。
// 一次遍历完成旧实现里 convertTo/split/merge 的约 10 趟全图扫描。
bool TextDetector::BuildDetectInput(const cv::Mat& image,
                                    const DetectParams& params,
                                    std::vector<float>& input_data, int& out_h,
                                    int& out_w, float& ratio_h,
                                    float& ratio_w) {
  if (image.empty()) return false;
  int h = image.rows;
  int w = image.cols;
  float ratio = 1.0f;
  const int max_side_len =
      params.max_side_len > 0 ? params.max_side_len : DefaultParams().max_side_len;

  // 缩放使最大边 <= max_side_len
  if (h > max_side_len || w > max_side_len) {
    if (h > w) {
      ratio = static_cast<float>(max_side_len) / h;
    } else {
      ratio = static_cast<float>(max_side_len) / w;
    }
  }

  int resize_h = std::max(static_cast<int>(std::round(h * ratio)), 1);
  int resize_w = std::max(static_cast<int>(std::round(w * ratio)), 1);

  // 填充到 32 的倍数（DB模型要求）
  resize_h = std::max(resize_h, 32);
  resize_w = std::max(resize_w, 32);
  if (resize_h % 32 != 0) resize_h = (resize_h / 32 + 1) * 32;
  if (resize_w % 32 != 0) resize_w = (resize_w / 32 + 1) * 32;

  // 记录缩放比例（用于后续坐标还原）
  ratio_h = static_cast<float>(h) / resize_h;
  ratio_w = static_cast<float>(w) / resize_w;

  cv::Mat resized;
  cv::resize(image, resized, cv::Size(resize_w, resize_h));

  // 如需则用零填充（resize 输出已经等于目标尺寸时不会触发）
  if (resized.rows < resize_h || resized.cols < resize_w) {
    cv::Mat padded = cv::Mat::zeros(resize_h, resize_w, resized.type());
    resized.copyTo(padded(cv::Rect(0, 0, resized.cols, resized.rows)));
    resized = padded;
  }

  // 只支持 8UC3 的融合快路径；其它类型（极少见）走通用 split/merge。
  input_data.assign(static_cast<size_t>(3) * resize_h * resize_w, 0.0f);
  if (resized.type() == CV_8UC3 && resized.isContinuous()) {
    FillNchwTensor(resized, input_data.data(), resize_h, resize_w,
                   /*rec_style=*/false);
  } else {
    cv::Mat float_img;
    resized.convertTo(float_img, CV_32FC3, 1.0 / 255.0);
    cv::Mat channels[3]; // SCREENSHOT_CMAKE_PATCHED: avoid STL ABI mismatch
    cv::split(float_img, channels);
    float mean_vals[] = {0.485f, 0.456f, 0.406f};  // R, G, B
    float std_vals[] = {0.229f, 0.224f, 0.225f};
    channels[2] = (channels[2] - mean_vals[0]) / std_vals[0];  // R
    channels[1] = (channels[1] - mean_vals[1]) / std_vals[1];  // G
    channels[0] = (channels[0] - mean_vals[2]) / std_vals[2];  // B
    for (int c = 0; c < 3; ++c) {
      std::memcpy(input_data.data() + static_cast<size_t>(c) * resize_h * resize_w,
                  channels[c].data,
                  static_cast<size_t>(resize_h) * resize_w * sizeof(float));
    }
  }
  out_h = resize_h;
  out_w = resize_w;
  return true;
}

// 跑一次检测推理，输出原始概率图（out_h × out_w，行优先）。
bool TextDetector::RunDetectInference(const float* input_data, int in_h,
                                      int in_w, std::vector<float>& out_data,
                                      int& out_h, int& out_w) {
  if (!session_ || input_data == nullptr || in_h <= 0 || in_w <= 0) return false;
  std::array<int64_t, 4> input_shape = {1, 3, in_h, in_w};
  auto memory_info = Ort::MemoryInfo::CreateCpu(
      OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
      memory_info, const_cast<float*>(input_data),
      static_cast<size_t>(3) * in_h * in_w, input_shape.data(),
      input_shape.size());

  const char* in_names[] = {input_name_.c_str()};
  const char* out_names[] = {output_name_.c_str()};

  try {
    PaddleOcrTimer t_infer("det.infer");
    auto output_tensors = session_.Run(
        Ort::RunOptions{nullptr}, in_names, &input_tensor, 1,
        out_names, 1);
    t_infer.Stop();

    // 获取输出数据
    const float* output_data = output_tensors[0].GetTensorMutableData<float>();
    auto output_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();
    if (output_shape.size() < 4) {
      OCR_LOG("RunDetectInference: unexpected output dim=%zu",
              output_shape.size());
      return false;
    }
    out_h = static_cast<int>(output_shape[2]);
    out_w = static_cast<int>(output_shape[3]);
    out_data.assign(output_data,
                    output_data + static_cast<size_t>(out_h) * out_w);
    return true;
  } catch (const Ort::Exception& e) {
    OCR_LOG("RunDetectInference: ORT exception: %s", e.what());
    return false;
  } catch (const std::exception& e) {
    OCR_LOG("RunDetectInference: exception: %s", e.what());
    return false;
  }
}

// 后处理：二值化 + 轮廓检测
std::vector<DetectedBox> TextDetector::PostProcess(
    const float* output, int out_h, int out_w,
    float ratio_h, float ratio_w, const DetectParams& params,
    int branch_index) {
  // 将输出转为 cv::Mat
  cv::Mat pred(out_h, out_w, CV_32FC1, const_cast<float*>(output));

  // 二值化
  cv::Mat binary;
  cv::threshold(pred, binary, static_cast<double>(params.thresh), 1.0,
                cv::THRESH_BINARY);
  binary.convertTo(binary, CV_8UC1);

  return BoxesFromBitmap(pred, binary, ratio_w, ratio_h, params, branch_index);
}

// 从二值化图中提取文本框
std::vector<DetectedBox> TextDetector::BoxesFromBitmap(
    const cv::Mat& pred, const cv::Mat& mask,
    float ratio_w, float ratio_h, const DetectParams& params,
    int branch_index) {
  // 轻微膨胀以改善轮廓检测
  cv::Mat dilated;
  cv::Mat kernel = cv::getStructuringElement(
      cv::MORPH_RECT, cv::Size(2, 2));
  cv::dilate(mask, dilated, kernel);

  // 查找轮廓
  // SCREENSHOT_CMAKE_PATCHED: use C API cvFindContours (STL ABI workaround)
  std::vector<std::vector<cv::Point>> contours;
  {
    CvMemStorage* _patched_storage = cvCreateMemStorage(0);
    CvSeq* _patched_cv_contours = nullptr;
    CvMat _patched_dilated_c = cvMat(dilated);
    cvFindContours(&_patched_dilated_c, _patched_storage,
                   &_patched_cv_contours, sizeof(CvContour),
                   CV_RETR_LIST, CV_CHAIN_APPROX_SIMPLE);
    for (CvSeq* _pc = _patched_cv_contours; _pc != nullptr;
         _pc = _pc->h_next) {
      std::vector<cv::Point> _patched_contour;
      _patched_contour.reserve(_pc->total);
      for (int _pi = 0; _pi < _pc->total; ++_pi) {
        CvPoint* _ppt = CV_GET_SEQ_ELEM(CvPoint, _pc, _pi);
        _patched_contour.push_back(cv::Point(_ppt->x, _ppt->y));
      }
      contours.push_back(std::move(_patched_contour));
    }
    cvReleaseMemStorage(&_patched_storage);
  }

  OCR_LOG("BoxesFromBitmap[b%d]: found %zu contours", branch_index,
          contours.size());

  std::vector<DetectedBox> results;
  int skipped_size = 0, skipped_score = 0, skipped_unclip = 0;
  const int min_box_size =
      params.min_box_size > 0 ? params.min_box_size : DefaultParams().min_box_size;

  for (size_t ci = 0; ci < contours.size(); ++ci) {
    const auto& contour = contours[ci];
    if (contour.size() < 4) {
      OCR_LOG("BoxesFromBitmap: contour[%zu] skipped - only %zu points",
              ci, contour.size());
      continue;
    }

    // 获取最小外接矩形
    cv::RotatedRect rect = cv::minAreaRect(_PatchedPointsToMat(contour)); // SCREENSHOT_CMAKE_PATCHED: Mat wrapper
    float short_side = std::min(rect.size.width, rect.size.height);
    if (short_side < static_cast<float>(min_box_size)) {
      skipped_size++;
      OCR_LOG("BoxesFromBitmap: contour[%zu] skipped - short_side=%.1f < %d",
              ci, short_side, min_box_size);
      continue;
    }
    if (params.min_box_area > 0 &&
        rect.size.width * rect.size.height < params.min_box_area) {
      skipped_size++;
      continue;
    }

    // 检查框的得分
    std::vector<cv::Point> box_pts(4);
    cv::Point2f vertices[4];
    rect.points(vertices);
    for (int i = 0; i < 4; ++i) {
      box_pts[i] = cv::Point(static_cast<int>(vertices[i].x),
                              static_cast<int>(vertices[i].y));
    }

    float score = BoxScore(pred, box_pts);
    if (score < static_cast<float>(params.box_thresh)) {
      skipped_score++;
      continue;
    }

    // Unclip（扩展多边形）
    std::vector<cv::Point> unclip_box;
    Unclip(box_pts, unclip_box, static_cast<float>(params.unclip_ratio));
    if (unclip_box.empty()) {
      skipped_unclip++;
      continue;
    }

    // 从扩展后的多边形获取最终外接矩形
    cv::RotatedRect final_rect = cv::minAreaRect(_PatchedPointsToMat(unclip_box)); // SCREENSHOT_CMAKE_PATCHED: Mat wrapper
    if (std::min(final_rect.size.width, final_rect.size.height) < 5.0f) {
      OCR_LOG("BoxesFromBitmap: contour[%zu] final too small: %.1fx%.1f",
              ci, final_rect.size.width, final_rect.size.height);
      continue;
    }

    cv::Point2f final_vertices[4];
    final_rect.points(final_vertices);

    // 缩放回原图坐标
    std::vector<cv::Point> result_box(4);
    for (int i = 0; i < 4; ++i) {
      result_box[i].x = static_cast<int>(
          std::round(final_vertices[i].x * ratio_w));
      result_box[i].y = static_cast<int>(
          std::round(final_vertices[i].y * ratio_h));
    }

    DetectedBox db;
    db.points = std::move(result_box);
    db.branch_index = branch_index;
    db.score = score;
    results.push_back(std::move(db));
  }

  OCR_LOG("BoxesFromBitmap[b%d]: results: %zu (skipped: size=%d score=%d "
          "unclip=%d)",
          branch_index, results.size(), skipped_size, skipped_score,
          skipped_unclip);

  // 按 y 坐标排序（从上到下），再按 x 排序（从左到右）
  std::sort(results.begin(), results.end(),
            [](const DetectedBox& a, const DetectedBox& b) {
              int min_ya = std::min({a.points[0].y, a.points[1].y,
                                      a.points[2].y, a.points[3].y});
              int min_yb = std::min({b.points[0].y, b.points[1].y,
                                      b.points[2].y, b.points[3].y});
              if (min_ya != min_yb) return min_ya < min_yb;
              int min_xa = std::min({a.points[0].x, a.points[1].x,
                                      a.points[2].x, a.points[3].x});
              int min_xb = std::min({b.points[0].x, b.points[1].x,
                                      b.points[2].x, b.points[3].x});
              return min_xa < min_xb;
            });

  return results;
}

// 计算多边形区域内的平均得分
// 使用多边形掩码计算平均分数（PaddleOCR "slow" 模式）。
// 比外接矩形平均更准确，因为它排除了文本多边形外的背景像素。
float TextDetector::BoxScore(const cv::Mat& pred,
                             const std::vector<cv::Point>& box) {
  int xmin = std::max(0, std::min({box[0].x, box[1].x, box[2].x, box[3].x}));
  int xmax = std::min(pred.cols - 1,
                      std::max({box[0].x, box[1].x, box[2].x, box[3].x}));
  int ymin = std::max(0, std::min({box[0].y, box[1].y, box[2].y, box[3].y}));
  int ymax = std::min(pred.rows - 1,
                      std::max({box[0].y, box[1].y, box[2].y, box[3].y}));

  if (xmin >= xmax || ymin >= ymax) return 0.0f;

  // 裁剪感兴趣区域
  cv::Mat roi = pred(cv::Rect(xmin, ymin, xmax - xmin + 1, ymax - ymin + 1));

  // 构建偏移到 ROI 坐标系的多边形掩码
  std::vector<cv::Point> shifted_box;
  shifted_box.reserve(box.size());
  for (const auto& pt : box) {
    shifted_box.emplace_back(pt.x - xmin, pt.y - ymin);
  }

  cv::Mat mask = cv::Mat::zeros(roi.size(), CV_8UC1);
  // SCREENSHOT_CMAKE_PATCHED: use const Point** overload (STL ABI workaround)
  cv::Point* _patched_poly_pts = shifted_box.data();
  int _patched_poly_n = static_cast<int>(shifted_box.size());
  cv::fillPoly(mask, const_cast<const cv::Point**>(&_patched_poly_pts),
               &_patched_poly_n, 1, cv::Scalar(1));

  // 仅计算掩码区域的平均得分
  return static_cast<float>(cv::mean(roi, mask)[0]);
}

// Unclip：扩展多边形，使检测框覆盖更完整的文本区域
// 使用鞋带公式计算多边形面积和周长，保留有符号面积以判断绕行方向
void TextDetector::Unclip(const std::vector<cv::Point>& in_poly,
                          std::vector<cv::Point>& out_poly,
                          float unclip_ratio) {
  // 使用鞋带公式计算多边形面积和周长。
  // 保留有符号面积以判断绕行方向。
  float signed_area = 0.0f;
  float perimeter = 0.0f;
  int n = static_cast<int>(in_poly.size());
  for (int i = 0; i < n; ++i) {
    int j = (i + 1) % n;
    signed_area += static_cast<float>(in_poly[i].x * in_poly[j].y -
                                      in_poly[j].x * in_poly[i].y);
    float dx = static_cast<float>(in_poly[j].x - in_poly[i].x);
    float dy = static_cast<float>(in_poly[j].y - in_poly[i].y);
    perimeter += std::sqrt(dx * dx + dy * dy);
  }
  float area = std::abs(signed_area) / 2.0f;

  if (perimeter < 1.0f || area < 1.0f) {
    out_poly = in_poly;
    return;
  }

  // 计算扩展距离
  float distance = area * unclip_ratio / perimeter;

  // 根据绕行方向确定外法线符号。
  // 在图像坐标系（y 向下）中，鞋带面积 > 0 表示顺时针。
  // 对于顺时针多边形，外法线在边的右侧：(dy, -dx)。
  // 对于逆时针多边形，外法线在边的左侧：(-dy, dx)。
  float normal_sign = (signed_area > 0) ? 1.0f : -1.0f;

  // 沿相邻两条边的法线平均值（斜接偏移）向外扩展每个顶点。
  std::vector<cv::Point2f> expanded(n);
  for (int i = 0; i < n; ++i) {
    int prev = (i - 1 + n) % n;
    int next = (i + 1) % n;

    // 边向量（归一化）
    float e1x = static_cast<float>(in_poly[i].x - in_poly[prev].x);
    float e1y = static_cast<float>(in_poly[i].y - in_poly[prev].y);
    float e2x = static_cast<float>(in_poly[next].x - in_poly[i].x);
    float e2y = static_cast<float>(in_poly[next].y - in_poly[i].y);

    float len1 = std::sqrt(e1x * e1x + e1y * e1y);
    float len2 = std::sqrt(e2x * e2x + e2y * e2y);
    if (len1 > 0) { e1x /= len1; e1y /= len1; }
    if (len2 > 0) { e2x /= len2; e2y /= len2; }

    // 外法线——方向取决于绕行顺序。
    // 右法线：(dy, -dx)，左法线：(-dy, dx)
    float n1x = normal_sign * e1y;
    float n1y = normal_sign * (-e1x);
    float n2x = normal_sign * e2y;
    float n2y = normal_sign * (-e2x);

    // 平均法线（斜接）
    float avg_nx = n1x + n2x;
    float avg_ny = n1y + n2y;
    float avg_len = std::sqrt(avg_nx * avg_nx + avg_ny * avg_ny);
    if (avg_len > 0) { avg_nx /= avg_len; avg_ny /= avg_len; }

    expanded[i].x = static_cast<float>(in_poly[i].x) + avg_nx * distance;
    expanded[i].y = static_cast<float>(in_poly[i].y) + avg_ny * distance;
  }

  // 验证扩展是否确实增加了面积；如果没有，说明法线方向错误——翻转。
  float new_signed_area = 0.0f;
  for (int i = 0; i < n; ++i) {
    int j = (i + 1) % n;
    new_signed_area += expanded[i].x * expanded[j].y -
                       expanded[j].x * expanded[i].y;
  }
  float new_area = std::abs(new_signed_area) / 2.0f;
  if (new_area < area) {
    // 扩展方向错误——翻转法线并重新计算。
    OCR_LOG("Unclip: WARNING - expansion decreased area (%.1f -> %.1f), "
            "flipping normals", area, new_area);
    for (int i = 0; i < n; ++i) {
      int prev = (i - 1 + n) % n;
      int next = (i + 1) % n;
      float e1x = static_cast<float>(in_poly[i].x - in_poly[prev].x);
      float e1y = static_cast<float>(in_poly[i].y - in_poly[prev].y);
      float e2x = static_cast<float>(in_poly[next].x - in_poly[i].x);
      float e2y = static_cast<float>(in_poly[next].y - in_poly[i].y);
      float len1 = std::sqrt(e1x * e1x + e1y * e1y);
      float len2 = std::sqrt(e2x * e2x + e2y * e2y);
      if (len1 > 0) { e1x /= len1; e1y /= len1; }
      if (len2 > 0) { e2x /= len2; e2y /= len2; }
      float n1x = -normal_sign * e1y;
      float n1y = -normal_sign * (-e1x);
      float n2x = -normal_sign * e2y;
      float n2y = -normal_sign * (-e2x);
      float avg_nx = n1x + n2x;
      float avg_ny = n1y + n2y;
      float avg_len = std::sqrt(avg_nx * avg_nx + avg_ny * avg_ny);
      if (avg_len > 0) { avg_nx /= avg_len; avg_ny /= avg_len; }
      expanded[i].x = static_cast<float>(in_poly[i].x) + avg_nx * distance;
      expanded[i].y = static_cast<float>(in_poly[i].y) + avg_ny * distance;
    }
  }

  // 四舍五入为整数坐标
  out_poly.resize(n);
  for (int i = 0; i < n; ++i) {
    out_poly[i].x = static_cast<int>(std::round(expanded[i].x));
    out_poly[i].y = static_cast<int>(std::round(expanded[i].y));
  }
}

// ============================================================================
// 文本识别器 TextRecognizer 实现
// 基于 CRNN + CTC 模型的文字识别
// ============================================================================

TextRecognizer::TextRecognizer()
    : env_(ORT_LOGGING_LEVEL_WARNING, "OcrRec") {}
TextRecognizer::~TextRecognizer() = default;

// 初始化识别模型和字典
bool TextRecognizer::Initialize(const std::string& model_path,
                                const std::string& dict_path) {
  Ort::SessionOptions session_options;
  session_options.SetIntraOpNumThreads(SessionThreadCount());
  session_options.SetGraphOptimizationLevel(
      GraphOptimizationLevel::ORT_ENABLE_ALL);

#ifdef _WIN32
  std::wstring wide_path = ToWString(model_path);
  // 让 Ort::Exception 向上传播，以便 OcrEngine 捕获错误信息
  session_ = Ort::Session(env_, wide_path.c_str(), session_options);
#else
  session_ = Ort::Session(env_, model_path.c_str(), session_options);
#endif
  Ort::AllocatorWithDefaultOptions alloc;
  auto in_name = session_.GetInputNameAllocated(0, alloc);
  auto out_name = session_.GetOutputNameAllocated(0, alloc);
  input_name_ = in_name.get();
  output_name_ = out_name.get();

  return LoadDict(dict_path);
}

// 加载字典文件
bool TextRecognizer::LoadDict(const std::string& dict_path) {
  OCR_LOG("LoadDict: path='%s'", dict_path.c_str());

  // 清除之前加载的字典（支持重复初始化）
  dict_.clear();

  // 以二进制模式打开以检测编码/BOM。
  // 注意：Windows 上 std::ifstream 的 char* 构造函数按系统 ANSI 代码页
  //（如 GBK）解释路径，UTF-8 编码的中文路径会打开失败。
  // 因此先转为宽字符路径，使用 MSVC 的 wchar_t 构造重载。
#ifdef _WIN32
  std::wstring wide_dict_path = ToWString(dict_path);
  // 注意：std::ifstream(std::wstring) 是 MSVC 私有扩展（llvm-mingw 的
  // libc++ 没有该重载）；std::filesystem::path 是 C++17 标准构造，
  // MSVC / libc++ 均支持，且在 Windows 上原生走宽字符 API（中文路径 OK）。
  std::ifstream file(std::filesystem::path(wide_dict_path),
                     std::ios::in | std::ios::binary);
#else
  // POSIX：文件系统原生使用 UTF-8 字节序列，直接构造即可。
  std::ifstream file(std::filesystem::path(dict_path),
                     std::ios::in | std::ios::binary);
#endif
  if (!file.is_open()) {
    OCR_LOG("LoadDict: FAILED to open file: %s", dict_path.c_str());
    fprintf(stderr, "Failed to open dict file: %s\n", dict_path.c_str());
    return false;
  }

  // 将整个文件读入缓冲区以进行编码分析
  std::string file_content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
  file.close();

  OCR_LOG("LoadDict: file size=%zu bytes", file_content.size());

  // 检查 BOM 标记
  if (file_content.size() >= 3 &&
      static_cast<unsigned char>(file_content[0]) == 0xEF &&
      static_cast<unsigned char>(file_content[1]) == 0xBB &&
      static_cast<unsigned char>(file_content[2]) == 0xBF) {
    OCR_LOG("LoadDict: WARNING - UTF-8 BOM detected, stripping 3 bytes");
    file_content = file_content.substr(3);
  } else if (file_content.size() >= 2 &&
      static_cast<unsigned char>(file_content[0]) == 0xFF &&
      static_cast<unsigned char>(file_content[1]) == 0xFE) {
    OCR_LOG("LoadDict: ERROR - UTF-16 LE BOM detected! Dict must be UTF-8.");
    return false;
  } else if (file_content.size() >= 2 &&
      static_cast<unsigned char>(file_content[0]) == 0xFE &&
      static_cast<unsigned char>(file_content[1]) == 0xFF) {
    OCR_LOG("LoadDict: ERROR - UTF-16 BE BOM detected! Dict must be UTF-8.");
    return false;
  } else {
    OCR_LOG("LoadDict: No BOM detected (assuming UTF-8)");
  }

  // 输出前 64 字节的十六进制转储以验证编码
  OCR_LOG("LoadDict: first 64 bytes hex: %s",
          HexDump(file_content.substr(0, 64)).c_str());

  // 验证文件是否为有效的 UTF-8
  std::string utf8_err = ValidateUtf8Detailed(file_content);
  if (!utf8_err.empty()) {
    OCR_LOG("LoadDict: WARNING - file is NOT valid UTF-8: %s", utf8_err.c_str());
    fprintf(stderr, "Dict file UTF-8 validation error: %s\n", utf8_err.c_str());
    // 仍然继续——SanitizeUtf8 会清理各个条目
  } else {
    OCR_LOG("LoadDict: file is valid UTF-8");
  }

  // 从内存缓冲区解析行
  std::istringstream stream(file_content);
  std::string line;
  int line_num = 0;
  while (std::getline(stream, line)) {
    line_num++;
    // 移除行尾的 \r（Windows 换行符）
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    // 验证每个字典条目是否为有效的 UTF-8
    std::string entry_err = ValidateUtf8Detailed(line);
    if (!entry_err.empty()) {
      OCR_LOG("LoadDict: WARNING - line %d has invalid UTF-8: %s | hex: %s",
              line_num, entry_err.c_str(), HexDump(line).c_str());
    }
    dict_.push_back(line);
  }

  OCR_LOG("LoadDict: loaded %zu dict entries from %d lines",
          dict_.size(), line_num);

  if (dict_.empty()) {
    OCR_LOG("LoadDict: ERROR - dict is empty after parsing");
    fprintf(stderr, "Dict file is empty: %s\n", dict_path.c_str());
    return false;
  }

  // 输出前 5 条和后 5 条以供验证
  OCR_LOG("LoadDict: first 5 entries:");
  for (size_t i = 0; i < std::min(dict_.size(), size_t(5)); ++i) {
    OCR_LOG("  [%zu] hex: %s", i, HexDump(dict_[i]).c_str());
  }
  if (dict_.size() > 10) {
    OCR_LOG("LoadDict: last 5 entries:");
    for (size_t i = dict_.size() - 5; i < dict_.size(); ++i) {
      OCR_LOG("  [%zu] hex: %s", i, HexDump(dict_[i]).c_str());
    }
  }

  return true;
}

// 识别每个文本区域中的文字（兼容旧签名：不启用文本条二次预处理）
std::vector<std::pair<std::string, float>> TextRecognizer::Recognize(
    const cv::Mat& image,
    const std::vector<std::vector<cv::Point>>& boxes) {
  RecognizeParams params;
  FillDefaultRecognize(&params);
  params.enhance_crops = 0;
  return Recognize(image, boxes, params);
}

std::vector<std::pair<std::string, float>> TextRecognizer::Recognize(
    const cv::Mat& image,
    const std::vector<std::vector<cv::Point>>& boxes,
    const RecognizeParams& params) {
  std::vector<std::pair<std::string, float>> results(boxes.size());
  if (!session_ || dict_.empty()) return results;

  // 各框识别彼此独立（Session::Run 可重入），并行执行；
  // 结果按下标回填，顺序与串行完全一致。
  ParallelFor(boxes.size(), [&](size_t bi) {
    if (boxes[bi].size() < 3) {
      results[bi] = {"", 0.0f};
      return;
    }
    results[bi] = RecognizeRegion(image, boxes[bi], params);
  }, RegionThreads());
  return results;
}

// 识别单个文本区域（一张图一个区域一次推理）
std::pair<std::string, float> TextRecognizer::RecognizeRegion(
    const cv::Mat& image, const std::vector<cv::Point>& box,
    const RecognizeParams& params) {
  if (!session_ || dict_.empty()) return {"", 0.0f};

  PaddleOcrTimer t_total("rec.total");
  cv::Mat crop;
  {
    PaddleOcrTimer t_crop("rec.crop");
    crop = PreprocessCrop(image, box, params);
  }
  if (crop.empty()) return {"", 0.0f};

  int crop_h = crop.rows;
  int crop_w = crop.cols;

  // 转换为 NCHW 浮点张量（融合快路径：一次遍历完成归一化）
  std::vector<float> input_data(static_cast<size_t>(3) * crop_h * crop_w);
  if (crop.type() == CV_8UC3 && crop.isContinuous()) {
    FillNchwTensor(crop, input_data.data(), crop_h, crop_w,
                   /*rec_style=*/true);
  } else {
    cv::Mat float_img;
    crop.convertTo(float_img, CV_32FC3, 1.0 / 255.0);
    float_img = (float_img - 0.5f) / 0.5f;
    cv::Mat channels[3]; // SCREENSHOT_CMAKE_PATCHED: avoid STL ABI mismatch
    cv::split(float_img, channels);
    for (int c = 0; c < 3; ++c) {
      std::memcpy(input_data.data() + static_cast<size_t>(c) * crop_h * crop_w,
                  channels[c].data,
                  static_cast<size_t>(crop_h) * crop_w * sizeof(float));
    }
  }

  std::array<int64_t, 4> input_shape = {1, 3, crop_h, crop_w};
  OCR_LOG("RecognizeRegion: input tensor shape=[1,3,%d,%d]", crop_h, crop_w);
  auto memory_info = Ort::MemoryInfo::CreateCpu(
      OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
      memory_info, input_data.data(), input_data.size(),
      input_shape.data(), input_shape.size());

  const char* in_names[] = {input_name_.c_str()};
  const char* out_names[] = {output_name_.c_str()};

  try {
    PaddleOcrTimer t_infer("rec.infer");
    auto output_tensors = session_.Run(
        Ort::RunOptions{nullptr}, in_names, &input_tensor, 1,
        out_names, 1);
    t_infer.Stop();

    float* output_data =
        output_tensors[0].GetTensorMutableData<float>();
    auto out_shape =
        output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();

    // 输出形状: [batch, seq_len, num_classes] (3D)
    //       或: [seq_len, num_classes] (2D，batch 已被压缩)
    // 注意：out_shape[0] 是 batch（通常为1），不是 seq_len！
    int seq_len, num_classes;
    if (out_shape.size() == 3) {
      seq_len = static_cast<int>(out_shape[1]);      // [1, seq_len, classes]
      num_classes = static_cast<int>(out_shape[2]);
    } else if (out_shape.size() == 2) {
      seq_len = static_cast<int>(out_shape[0]);      // [seq_len, classes]
      num_classes = static_cast<int>(out_shape[1]);
    } else {
      OCR_LOG("RecognizeRegion: unexpected output dim=%zu",
              out_shape.size());
      return {"", 0.0f};
    }

    auto decoded = DecodeOutput(output_data, seq_len, num_classes);
    if (decoded.second < static_cast<float>(params.min_confidence)) {
      return {"", decoded.second};
    }
    return decoded;
  } catch (const Ort::Exception& e) {
    fprintf(stderr, "Recognition inference failed: %s\n", e.what());
    return {"", 0.0f};
  }
}

// 预处理裁剪区域：可选二次增强 → 缩放到固定高度 → 归一化
cv::Mat TextRecognizer::PreprocessCrop(
    const cv::Mat& image, const std::vector<cv::Point>& box,
    const RecognizeParams& params) {
  const int img_h = params.img_height > 0 ? params.img_height : 48;
  const int max_w = params.max_img_width > 0 ? params.max_img_width : 2048;

  cv::Mat crop = GetRotatedCropImage(image, box);
  if (crop.empty()) return crop;

  OCR_LOG("PreprocessCrop: crop size %dx%d", crop.cols, crop.rows);

  // 文本条二次预处理（二值化 / 增强）：把压在复杂背景上的字洗成
  // 黑字白底，识别器就不容易被背景纹理带偏而丢字。
  if (params.enhance_crops) {
    cv::Mat enhanced = PrepareCropForRecognition(crop, params.crop_preprocess);
    if (!enhanced.empty()) crop = enhanced;
  }

  // 缩放到固定高度，保持宽高比
  int h = crop.rows;
  int w = crop.cols;
  if (h <= 0 || w <= 0) return cv::Mat();
  float ratio = static_cast<float>(img_h) / h;
  int new_w = std::max(static_cast<int>(w * ratio), 1);
  // 超宽文本限幅：模型高度维度是固定的，只能横向压缩，
  // 否则会生成巨大的输入张量（甚至超过模型训练时的宽高比）。
  new_w = std::min(new_w, max_w);

  OCR_LOG("PreprocessCrop: resize to %dx%d (ratio=%.4f)", new_w, img_h, ratio);

  cv::Mat resized;
  cv::resize(crop, resized, cv::Size(new_w, img_h));

  // 宽度至少填充到 32（右侧补零；归一化在 RecognizeRegion 里融合完成）
  int pad_w = std::max(new_w, 32);
  if (new_w < pad_w) {
    cv::Mat padded = cv::Mat::zeros(img_h, pad_w, resized.type());
    resized.copyTo(padded(cv::Rect(0, 0, new_w, img_h)));
    resized = padded;
  }

  OCR_LOG("PreprocessCrop: final tensor %dx%d", resized.cols, resized.rows);
  return resized;
}

// CTC 贪心解码
std::pair<std::string, float> TextRecognizer::DecodeOutput(
    const float* data, int seq_len, int num_classes) {
  // CTC 贪心解码
  std::string text;
  float total_conf = 0.0f;
  int valid_count = 0;
  int last_idx = 0;  // CTC blank = 0

  OCR_LOG("DecodeOutput: seq_len=%d, num_classes=%d, dict_size=%zu",
          seq_len, num_classes, dict_.size());

  for (int t = 0; t < seq_len; ++t) {
    const float* row = data + t * num_classes;

    // 寻找 argmax（最大值索引）
    int max_idx = 0;
    float max_val = row[0];
    for (int c = 1; c < num_classes; ++c) {
      if (row[c] > max_val) {
        max_val = row[c];
        max_idx = c;
      }
    }

    // CTC：跳过 blank(0) 和重复字符
    if (max_idx != 0 && max_idx != last_idx) {
      int dict_idx = max_idx - 1;  // 字典从索引0开始，模型输出从1开始
      if (dict_idx >= 0 && dict_idx < static_cast<int>(dict_.size())) {
        text += dict_[dict_idx];
        // 置信度来自 log-softmax 输出：exp(log_prob)
        // PP-OCR 识别模型输出的是对数概率，不是原始 logits。
        float conf = std::exp(row[max_idx]);
        if (conf > 1.0f) conf = 1.0f;  // 数值安全裁剪
        if (conf < 0.0f) conf = 0.0f;
        total_conf += conf;
        valid_count++;
      } else {
        OCR_LOG("DecodeOutput: WARNING - dict_idx %d out of range [0, %zu)",
                dict_idx, dict_.size());
      }
    }
    // 始终更新 last_idx 以正确处理 CTC 合并：
    // blank 重置去重，所以 A-blank-A 应产生 "AA"
    last_idx = max_idx;
  }

  float avg_conf = valid_count > 0 ? total_conf / valid_count : 0.0f;

  // 调试：记录清理前的原始文本
  OCR_LOG("DecodeOutput: raw text length=%zu bytes, conf=%.6f, valid_count=%d",
          text.size(), avg_conf, valid_count);
  OCR_LOG("DecodeOutput: raw text hex: %s", HexDump(text).c_str());

  // 验证原始文本
  std::string raw_err = ValidateUtf8Detailed(text);
  if (!raw_err.empty()) {
    OCR_LOG("DecodeOutput: WARNING - raw text has invalid UTF-8: %s", raw_err.c_str());
  }

  // 清理 UTF-8
  std::string sanitized = SanitizeUtf8(text);

  // 验证清理后的文本
  std::string san_err = ValidateUtf8Detailed(sanitized);
  if (!san_err.empty()) {
    OCR_LOG("DecodeOutput: ERROR - sanitized text STILL has invalid UTF-8: %s", san_err.c_str());
    OCR_LOG("DecodeOutput: sanitized hex: %s", HexDump(sanitized).c_str());
  } else {
    OCR_LOG("DecodeOutput: sanitized text is valid UTF-8, length=%zu bytes",
            sanitized.size());
  }

  return {sanitized, avg_conf};
}

// ============================================================================
// OCR 引擎 OcrEngine 实现
// 组合检测器和识别器，提供完整的 OCR 流程
// ============================================================================

OcrEngine::OcrEngine() {
  // 默认选项必须显式初始化：OcrOptions 是 POD，不会自动清零，
  // 若不填，未调 SetDefaultOptions 的调用方会读到随机参数。
  FillDefaultOptions(&default_options_);
}
OcrEngine::~OcrEngine() = default;

// 初始化引擎：加载检测模型、识别模型和字典
bool OcrEngine::Initialize(const std::string& det_model_path,
                           const std::string& rec_model_path,
                           const std::string& dict_path) {
  last_error_.clear();
  OCR_LOG("Initialize: det_model='%s'", det_model_path.c_str());
  OCR_LOG("Initialize: rec_model='%s'", rec_model_path.c_str());
  OCR_LOG("Initialize: dict_path='%s'", dict_path.c_str());

  // 验证路径是否为有效的 UTF-8
  std::string det_err = ValidateUtf8Detailed(det_model_path);
  if (!det_err.empty()) OCR_LOG("Initialize: det_model_path invalid UTF-8: %s", det_err.c_str());
  std::string rec_err = ValidateUtf8Detailed(rec_model_path);
  if (!rec_err.empty()) OCR_LOG("Initialize: rec_model_path invalid UTF-8: %s", rec_err.c_str());
  std::string dict_err = ValidateUtf8Detailed(dict_path);
  if (!dict_err.empty()) OCR_LOG("Initialize: dict_path invalid UTF-8: %s", dict_err.c_str());

  try {
    if (!detector_.Initialize(det_model_path)) {
      last_error_ = "Failed to load detection model: " + det_model_path;
      OCR_LOG("Initialize: detector init failed: %s", last_error_.c_str());
      return false;
    }
    OCR_LOG("Initialize: detector loaded successfully");
    if (!recognizer_.Initialize(rec_model_path, dict_path)) {
      last_error_ = "Failed to load recognition model or dict: " + rec_model_path;
      OCR_LOG("Initialize: recognizer init failed: %s", last_error_.c_str());
      return false;
    }
    OCR_LOG("Initialize: recognizer loaded successfully");
  } catch (const std::exception& e) {
    last_error_ = std::string("Exception: ") + e.what();
    OCR_LOG("Initialize: exception: %s", last_error_.c_str());
    return false;
  }
  initialized_ = true;
  OCR_LOG("Initialize: success");
  return true;
}

// ===========================================================================
// 多分支检测的配置表
//
// 每一路 = 一种预处理 + 一套检测参数。分支只在本底检测参数上做
// 「定向偏移」，因此宿主调 thresh 时每一路都会跟着变。
// ===========================================================================
namespace {

struct BranchConfig {
  uint32_t flag = 0;
  std::string name;
  PreprocessParams prep;
  DetectParams det;
  // 该分支的图能不能拿去喂识别器。
  // 二值化/白字提取是为「让检测器看清结构」服务的，它们的输出已经丢掉了
  // 自然灰度分布，拿去做识别反而会大量乱码；而放大/锐化/轻度增强既
  // 有利检测也不伤识别。因此只对后者开这个位。
  bool rec_source = true;
};

BranchConfig MakeBranchConfig(uint32_t flag, const OcrOptions& base) {
  BranchConfig cfg;
  cfg.flag = flag;
  FillDefaultPreprocess(&cfg.prep);
  cfg.det = base.det;

  switch (flag) {
    case kBranchRaw:
      cfg.name = "raw";
      break;

    case kBranchAdaptiveBin:
      // 自适应阈值只看局部邻域，对光照渐变、压景物的白字最鲁棒。
      cfg.name = "adaptive_bin";
      cfg.prep.binarize_mode = kBinarizeAdaptiveGaussInv;
      cfg.prep.color_mode = kColorGray3;
      cfg.rec_source = false;
      break;

    case kBranchOtsuBin:
      cfg.name = "otsu_bin";
      cfg.prep.binarize_mode = kBinarizeOtsuInv;
      cfg.prep.color_mode = kColorGray3;
      cfg.rec_source = false;
      break;

    case kBranchClahe:
      cfg.name = "clahe";
      cfg.prep.enhance_mode = kEnhanceClahe;
      cfg.prep.clahe_clip_limit = 3.0;
      break;

    case kBranchUpscale2x:
      // 放大后必须同步放宽输入边长，否则又会被缩回原分辨率，白放大一场。
      cfg.name = "upscale2x";
      cfg.prep.scale_percent = 200;
      cfg.det.max_side_len = base.det.max_side_len * 2;
      break;

    case kBranchUpscale3x:
      // 3 倍放大：20px 级小字在 2x 下仍会偶发繁简误读（篮→籃），
      // 3x 时笔画细节充足。速度比 2x 慢一档，按需开启。
      // 注意：检测输入必须给足 3×base。实测降到 2×base（文字在检测
      // 输入里仍有 ~53px）会丢行首细笔画「一」的召回（"一舍篮球场…"
      // 退化成 "合篮球场…"）—— 细横笔画的 DB 响应本就微弱，靠的就是
      // 检测输入里的绝对尺寸。识别用的也是整张 3x 图，不冲突。
      cfg.name = "upscale3x";
      cfg.prep.scale_percent = 300;
      cfg.det.max_side_len = base.det.max_side_len * 3;
      break;

    case kBranchSensitive:
      // 不改变图像，只把检测阈值放宽，拾回临界响应的小字。
      // 图像与原图完全相同，因此也不需要再当一遍识别源。
      cfg.name = "sensitive";
      cfg.det.thresh = std::max(0.10, base.det.thresh * 0.80);
      cfg.det.box_thresh = std::max(0.15, base.det.box_thresh * 0.82);
      cfg.det.unclip_ratio = base.det.unclip_ratio + 0.3;
      cfg.rec_source = false;
      break;

    case kBranchSharpen:
      cfg.name = "sharpen";
      cfg.prep.enhance_mode = kEnhanceSharpen;
      break;

    case kBranchDenoiseClahe:
      cfg.name = "denoise_clahe";
      cfg.prep.enhance_mode = kEnhanceDenoiseClahe;
      break;

    case kBranchWhiteText:
      // 监控叠加字幕专用：先把「高亮度 + 低饱和度」的像素抽出来，
      // 再做局部二值化，白字与树丛/广告牌的分离度会提升一个量级。
      cfg.name = "white_text";
      cfg.prep.enhance_mode = kEnhanceWhiteText;
      cfg.prep.binarize_mode = kBinarizeAdaptiveGaussInv;
      cfg.prep.adaptive_block_size = 21;
      cfg.prep.adaptive_c = 8.0;
      cfg.prep.color_mode = kColorGray3;
      cfg.rec_source = false;
      break;

    default:
      cfg.name = "custom";
      break;
  }
  return cfg;
}

// 分支顺序固定，保证同一 mask 在任何平台得列同样的 branch_index。
std::vector<BranchConfig> BuildBranchConfigs(const OcrOptions& options) {
  static const uint32_t kOrder[] = {
      kBranchRaw,        kBranchAdaptiveBin,  kBranchOtsuBin,
      kBranchClahe,      kBranchUpscale2x,    kBranchUpscale3x,
      kBranchSensitive,  kBranchSharpen,      kBranchDenoiseClahe,
      kBranchWhiteText,
  };
  std::vector<BranchConfig> out;
  for (uint32_t flag : kOrder) {
    if (options.merge.branches & flag) {
      out.push_back(MakeBranchConfig(flag, options));
    }
  }
  if (out.empty()) out.push_back(MakeBranchConfig(kBranchRaw, options));
  return out;
}

}  // namespace

// ===========================================================================
// OcrEngine：选项管理
// ===========================================================================

void OcrEngine::SetDefaultOptions(const OcrOptions& options) {
  default_options_ = options;
  SanitizeOptions(&default_options_);
  has_explicit_default_ = true;
  OCR_LOG("SetDefaultOptions: branches=%s stitch=%d enhance_crops=%d "
          "det(%.2f/%.2f/%.1f/%d)",
          BranchFlagsName(default_options_.merge.branches).c_str(),
          default_options_.merge.stitch_same_line,
          default_options_.rec.enhance_crops, default_options_.det.thresh,
          default_options_.det.box_thresh, default_options_.det.unclip_ratio,
          default_options_.det.max_side_len);
}

void OcrEngine::ClearDefaultOptions() {
  FillDefaultOptions(&default_options_);
  has_explicit_default_ = false;
}

// ===========================================================================
// OcrEngine：原子操作（宿主编排自定义管线用）
// ===========================================================================

std::vector<DetectedBox> OcrEngine::DetectOnImage(const cv::Mat& image,
                                                  const DetectParams& params) {
  if (!initialized_ || image.empty()) return {};
  DetectParams p = params;
  p.struct_size = static_cast<int32_t>(sizeof(DetectParams));
  return detector_.DetectScored(image, p, 0);
}

// 合并 + 拼接 + 识别窗构造（RunPipeline 与原子操作共用的收尾流程）。
// 输出的 points 已经是「识别窗」：基底取最贴合字带的成员框，含四周与
// 行向外扩，可直接裁剪喂识别器。
static std::vector<MergedBox> MergeAndBuildWindows(
    const std::vector<DetectedBox>& boxes, const MergeParams& m,
    int image_width, int image_height) {
  std::vector<MergedBox> regions = MergeDetectedBoxes(boxes, m);
  regions = StitchSameLine(regions, m);
  for (auto& region : regions) {
    if (region.points.size() < 3) continue;
    region.points = BuildRecognitionWindow(region, m.region_pad_ratio,
                                           m.region_pad_long_ratio,
                                           image_width, image_height);
  }
  SortBoxesForReadingOrder(&regions, m.sort_order);
  if (m.max_regions > 0 &&
      static_cast<int>(regions.size()) > m.max_regions) {
    regions.resize(static_cast<size_t>(m.max_regions));
  }
  return regions;
}

std::vector<MergedBox> OcrEngine::MergeRegions(
    const std::vector<DetectedBox>& boxes, const MergeParams& params,
    int image_width, int image_height) {
  MergeParams m = params;
  m.struct_size = static_cast<int32_t>(sizeof(MergeParams));
  return MergeAndBuildWindows(boxes, m, image_width, image_height);
}

std::pair<std::string, float> OcrEngine::RecognizeBox(
    const cv::Mat& image, const std::vector<cv::Point>& box,
    const RecognizeParams& params) {
  if (!initialized_ || image.empty() || box.size() < 3) return {"", 0.0f};
  RecognizeParams p = params;
  p.struct_size = static_cast<int32_t>(sizeof(RecognizeParams));
  return recognizer_.RecognizeRegion(image, box, p);
}

cv::Mat OcrEngine::ApplyImageTransform(const cv::Mat& image,
                                       const PreprocessParams& params) {
  if (image.empty()) return image;
  PreprocessParams p = params;
  p.struct_size = static_cast<int32_t>(sizeof(PreprocessParams));
  return ApplyPreprocess(image, p);
}

double OcrEngine::ScoreVariant(int32_t variant_select, const std::string& text,
                               float confidence, double long_side,
                               double short_side, bool raw_crop) {
  return VariantSelectionScore(variant_select, text, confidence, long_side,
                               short_side, raw_crop);
}

// ===========================================================================
// OcrEngine：识别入口
// ===========================================================================

// 识别图片中的文字（使用当前默认选项）
std::vector<OcrBoxResult> OcrEngine::Recognize(const cv::Mat& image) {
  return Recognize(image, default_options_);
}

std::vector<OcrBoxResult> OcrEngine::Recognize(const cv::Mat& image,
                                               const OcrOptions& options) {
  if (!initialized_ || image.empty()) {
    OCR_LOG("Recognize: skipped (initialized=%d, image_empty=%d)",
            initialized_, image.empty());
    return {};
  }

  OcrOptions opts = options;
  SanitizeOptions(&opts);

  OCR_LOG("Recognize: image %dx%d, channels=%d, branches=%s",
          image.cols, image.rows, image.channels(),
          BranchFlagsName(opts.merge.branches).c_str());

  // ROI：只在矩形内跑管道，但把坐标加回原图偏移，保证对调用方而言
  // 坐标系始终是「送进来的那张图」。
  cv::Point roi_offset(0, 0);
  cv::Mat work = image;
  if (opts.roi_w > 0 && opts.roi_h > 0) {
    cv::Rect roi(opts.roi_x, opts.roi_y, opts.roi_w, opts.roi_h);
    roi &= cv::Rect(0, 0, image.cols, image.rows);
    if (roi.width >= 8 && roi.height >= 8) {
      work = image(roi).clone();
      roi_offset = cv::Point(roi.x, roi.y);
      OCR_LOG("Recognize: roi=%dx%d at (%d,%d)", roi.width, roi.height,
              roi.x, roi.y);
    } else {
      OCR_LOG("Recognize: roi too small (%dx%d), ignored", roi.width, roi.height);
    }
  }

  // 单路且不拼接时走历史路径，保证默认行为与旧版本逐字一致。
  const bool single_branch =
      opts.merge.branches == kBranchRaw && !opts.merge.stitch_same_line;
  auto results = single_branch ? RunSingleBranch(work, opts)
                               : RunPipeline(work, opts);

  // 统一做 UTF-8 安全网与日志。
  for (size_t i = 0; i < results.size(); ++i) {
    OcrBoxResult& r = results[i];
    if (roi_offset.x != 0 || roi_offset.y != 0) {
      for (auto& pt : r.box) {
        pt.x += roi_offset.x;
        pt.y += roi_offset.y;
      }
    }
    std::string err = ValidateUtf8Detailed(r.text);
    if (!err.empty()) {
      OCR_LOG("Recognize: ERROR - result[%zu] text has invalid UTF-8: %s",
              i, err.c_str());
      r.text = SanitizeUtf8(r.text);
    }
    OCR_LOG("Recognize: result[%zu] box=[(%d,%d)(%d,%d)(%d,%d)(%d,%d)] "
            "conf=%.4f text='%s'",
            i,
            r.box[0].x, r.box[0].y, r.box[1].x, r.box[1].y,
            r.box[2].x, r.box[2].y, r.box[3].x, r.box[3].y,
            r.confidence, r.text.c_str());
  }
  return results;
}

// 单分支兼容路径：检测 → 识别 → 拼装
std::vector<OcrBoxResult> OcrEngine::RunSingleBranch(
    const cv::Mat& image, const OcrOptions& options) {
  auto boxes = detector_.DetectScored(image, options.det, 0);
  OCR_LOG("RunSingleBranch: detected %zu text regions", boxes.size());
  if (boxes.empty()) return {};

  std::vector<std::vector<cv::Point>> plain;
  plain.reserve(boxes.size());
  for (const auto& db : boxes) plain.push_back(db.points);

  auto texts = recognizer_.Recognize(image, plain, options.rec);

  std::vector<OcrBoxResult> results;
  size_t count = std::min(plain.size(), texts.size());
  results.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    if (texts[i].first.empty()) continue;
    OcrBoxResult r;
    r.box = std::move(plain[i]);
    r.text = std::move(texts[i].first);
    r.confidence = texts[i].second;
    results.push_back(std::move(r));
  }
  return results;
}

// 原图框 → 分支图框（分支可能把图缩放过）
std::vector<cv::Point> OcrEngine::ScaleBoxToBranch(
    const std::vector<cv::Point>& box, double scale) {
  if (std::abs(scale - 1.0) < 1e-9) return box;
  std::vector<cv::Point> out;
  out.reserve(box.size());
  for (const auto& pt : box) {
    out.emplace_back(static_cast<int>(std::round(pt.x * scale)),
                     static_cast<int>(std::round(pt.y * scale)));
  }
  return out;
}

// ===========================================================================
// 放大分支（detail 分支）的检测 ROI 规划
//
// 放大分支的检测开销随输入面积线性增长（2x ≈ 4 倍、3x ≈ 9 倍于 1x），而
// 文字通常只占画面很小一块。先用 1x 分支把候选区全图扫出来，再让放大
// 分支只在「候选区 + 外扩」内检测：文字附近的像素一个不少，画面空白处
// 不再为 3x 分辨率买单。
//
// 合并规则：
//   1. 每个检出框先外扩（按短边 = 字高定）—— 文字行方向 2.5 倍字高
//      （1x 检测常把行尾几个字符切掉，留宽才能让放大分支补回来），
//      垂直方向 1 倍字高（DB 取上下文够用，给多了只是把上下景物拖进 3x
//      推理）。不能用框高定外扩：一个跨多行的碎块高度可达上百像素，
//      按它外扩会把 ROI 铺回半张图，优化就失去意义了；
//   2. 相交或间隙小于半个字高的矩形合并成一块（同一行的碎框、上下两行
//      字幕各算一块），避免因 ROI 边界把一行字切断；
//   3. 数量超过上限时，把面积最小的一块并给「并起来最省面积」的邻居，
//      防止 ROI 数量退化成逐框一次推理。
// =========================================================================
static cv::Rect InflateRect(const cv::Rect& r, int gap) {
  return cv::Rect(r.x - gap, r.y - gap, r.width + 2 * gap, r.height + 2 * gap);
}

static std::vector<cv::Rect> PlanDetailRois(
    const std::vector<DetectedBox>& boxes, int img_w, int img_h) {
  const cv::Rect image_rect(0, 0, img_w, img_h);
  std::vector<cv::Rect> rects;
  rects.reserve(boxes.size());
  for (const auto& db : boxes) {
    const cv::Rect r = BoxBoundingRect(db.points) & image_rect;
    if (r.width < 2 || r.height < 2) continue;
    const int em = std::min(r.width, r.height);
    // 水平（文字行方向）给足 2.5 倍字高：1x 检测常把行尾几个字符切掉，
    // 留宽一点才能让放大分支把它们补回来；垂直方向 1 倍字高就够 DB 取
    // 上下文，给多了只是把上下一整片景物拖进 3x 推理。
    const int pad_x = std::max(12, std::min(60, 5 * em / 2));
    const int pad_y = std::max(8, std::min(24, em));
    rects.push_back((cv::Rect(r.x - pad_x, r.y - pad_y, r.width + 2 * pad_x,
                              r.height + 2 * pad_y)) & image_rect);
  }
  if (rects.empty()) return rects;

  // 相交 / 近邻合并（O(n^2 × 轮数)，n 是 1x 检出框数，量级几十）。
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 0; i < rects.size() && !changed; ++i) {
      for (size_t j = i + 1; j < rects.size(); ++j) {
        const int gap = std::max(4, std::min(8, std::min(rects[i].height,
                                             rects[j].height) / 2));
        if ((InflateRect(rects[i], gap) & rects[j]).area() > 0) {
          rects[i] |= rects[j];
          rects.erase(rects.begin() + static_cast<long>(j));
          changed = true;
          break;
        }
      }
    }
  }

  // 最小尺寸：太小的 ROI 让 DB 失去上下文，也摊不平一次推理的固定开销。
  const int kMinW = 64, kMinH = 48;
  for (auto& r : rects) {
    if (r.width < kMinW) {
      r.x -= (kMinW - r.width) / 2;
      r.width = kMinW;
    }
    if (r.height < kMinH) {
      r.y -= (kMinH - r.height) / 2;
      r.height = kMinH;
    }
    r &= image_rect;
  }

  constexpr size_t kMaxDetailRois = 8;
  while (rects.size() > kMaxDetailRois) {
    size_t smallest = 0;
    for (size_t i = 1; i < rects.size(); ++i) {
      if (rects[i].area() < rects[smallest].area()) smallest = i;
    }
    size_t best = SIZE_MAX;
    long long best_area = std::numeric_limits<long long>::max();
    for (size_t i = 0; i < rects.size(); ++i) {
      if (i == smallest) continue;
      const long long merged_area =
          static_cast<long long>((rects[i] | rects[smallest]).area());
      if (merged_area < best_area) {
        best_area = merged_area;
        best = i;
      }
    }
    rects[best] |= rects[smallest];
    rects.erase(rects.begin() + static_cast<long>(smallest));
  }

  std::sort(rects.begin(), rects.end(), [](const cv::Rect& a, const cv::Rect& b) {
    if (a.y != b.y) return a.y < b.y;
    return a.x < b.x;
  });
  return rects;
}

// 诊断开关：置 1 时放大分支恢复「整图检测」的历史行为（对照/回滚用）。
static bool DetailFullImageEnabled() {
  static const bool enabled = [] {
    const char* v = std::getenv("PP_OCR_DETAIL_FULL");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return enabled;
}

// ===========================================================================
// 多分支完整管道
//
//   1. 每一路预处理 → 检测 → 把框换算回原图坐标
//      · 基础分支（1x）整图检测，负责「把字找全」；
//      · 放大分支（2x/3x）只在基础分支检出的 ROI 内外检测（见
//        PlanDetailRois），文字附近细节不变、空白区域不再按 9 倍面积付费。
//   2. 跨分支去重合并（同一区域只留一个框，但记住是哪几路贡献的）
//   3. 同行碎框拼接（把一行被切碎的字合成整行）
//   4. 择优识别：同一个区域在「原图 + 各分支图」上各识别一次，按
//      「结果宽度 vs 区域长宽比」打分取最优（见 VariantSelectionScore）
// ===========================================================================
std::vector<OcrBoxResult> OcrEngine::RunPipeline(const cv::Mat& image,
                                                 const OcrOptions& options) {
  PaddleOcrTimer t_pipeline("pipeline.total");
  const std::vector<BranchConfig> configs = BuildBranchConfigs(options);

  // 步骤 1a：每路预处理（彼此独立，并行执行）。
  // 基础分支（1x）顺手构建整图检测张量；放大分支不在这里构建 —— 整图 3x
  // 输入张量是 3 通道 × 3168 × 1472 × 4B ≈ 56MB，而它最终只会用到 ROI
  // 那一小块（步骤 1d）。
  //
  // 与推理解耦成两个阶段的理由：OpenCV（resize/自适应阈值等）与 ORT 各有
  // 自己的线程池，混在同一个并行 lane 里会互相抢核；分开后预处理吃满
  // OpenCV 池、推理吃满 ORT 池，实测检测阶段墙钟明显下降。
  std::vector<BranchOutput> slots(configs.size());
  std::vector<char> slot_ok(configs.size(), 0);
  {
    PaddleOcrTimer t("pipeline.preprocess_phase");
    ParallelFor(configs.size(), [&](size_t bi) {
    const BranchConfig& cfg = configs[bi];

    BranchOutput out;
    out.flag = cfg.flag;
    out.name = cfg.name;
    out.rec_source = cfg.rec_source;
    out.config_index = static_cast<int>(bi);

    double scale = 1.0;
    out.image = ApplyPreprocess(image, cfg.prep, &scale);
    out.scale = scale;
    if (out.image.empty()) {
      OCR_LOG("RunPipeline: branch[%zu '%s'] preprocess produced empty image",
              bi, cfg.name.c_str());
      return;
    }

    out.detail = scale > 1.0 + 1e-9 && !DetailFullImageEnabled();
    if (!out.detail) {
      float ratio_h = 1.0f, ratio_w = 1.0f;
      if (!detector_.BuildDetectInput(out.image, cfg.det, out.input, out.input_h,
                                      out.input_w, ratio_h, ratio_w)) {
        OCR_LOG("RunPipeline: branch[%zu '%s'] detect input build failed", bi,
                cfg.name.c_str());
        return;
      }
      out.ratio_h = ratio_h;
      out.ratio_w = ratio_w;
    }
    slots[bi] = std::move(out);
    slot_ok[bi] = 1;
    }, BranchThreads());
  }

  // 步骤 1b：按「输入张量内容」给**基础分支**分组。
  // 不同分支可能对同一张预处理图只用不同的后处理阈值（典型：raw 与
  // sensitive 的预处理产物逐字节相同，只有 thresh/box_thresh/unclip
  // 不同）—— 整组只需跑一次模型推理，组内每个分支各用自己的
  // DetectParams 做后处理。分组只按内容判等（哈希 + 逐字节确认），
  // 与分支顺序无关，不改变 branch_index 与 all_boxes 的最终顺序。
  struct DetectGroup {
    std::vector<size_t> members;  // 配置下标，升序（members[0] 是推理担当）
  };
  std::vector<DetectGroup> groups;
  std::vector<size_t> detail_branches;
  for (size_t bi = 0; bi < configs.size(); ++bi) {
    if (!slot_ok[bi]) continue;
    if (slots[bi].detail) {
      detail_branches.push_back(bi);
      continue;
    }
    const BranchOutput& cand = slots[bi];
    bool placed = false;
    for (auto& g : groups) {
      const BranchOutput& leader = slots[g.members[0]];
      if (leader.input_h != cand.input_h || leader.input_w != cand.input_w) {
        continue;
      }
      if (!TensorsBitwiseEqual(leader.input, cand.input)) continue;
      g.members.push_back(bi);
      placed = true;
      break;
    }
    if (!placed) {
      DetectGroup g;
      g.members.push_back(bi);
      groups.push_back(std::move(g));
    }
  }
  OCR_LOG("RunPipeline: %zu branches -> %zu base groups (dedup %zu), %zu detail",
          configs.size(), groups.size(),
          configs.size() - detail_branches.size() - groups.size(),
          detail_branches.size());

  // 步骤 1c：每组推理一次；组内每个分支用自己的 DetectParams 后处理，
  // 并把框坐标换算回原图。组内后处理是轻量活（概率图阈值 + 轮廓），
  // 串行做不足以抵消并行调度开销。
  {
    PaddleOcrTimer t("pipeline.infer_phase");
    ParallelFor(groups.size(), [&](size_t gi) {
      const std::vector<size_t>& members = groups[gi].members;
      const size_t leader = members[0];
      std::vector<float> out_data;
      int out_h = 0, out_w = 0;
      if (!detector_.RunDetectInference(slots[leader].input.data(),
                                        slots[leader].input_h,
                                        slots[leader].input_w, out_data, out_h,
                                        out_w)) {
        return;
      }
      for (size_t bi : members) {
        slots[bi].boxes = detector_.PostProcess(
            out_data.data(), out_h, out_w, slots[bi].ratio_h, slots[bi].ratio_w,
            configs[bi].det, static_cast<int>(bi));

        // 框坐标从分支图换算回原图
        if (std::abs(slots[bi].scale - 1.0) > 1e-9) {
          for (auto& db : slots[bi].boxes) {
            for (auto& pt : db.points) {
              pt.x = static_cast<int>(std::round(pt.x / slots[bi].scale));
              pt.y = static_cast<int>(std::round(pt.y / slots[bi].scale));
            }
          }
        }

        OCR_LOG("RunPipeline: branch[%zu '%s'] scale=%.2f detected %zu boxes",
                bi, configs[bi].name.c_str(), slots[bi].scale,
                slots[bi].boxes.size());
      }
    }, BranchThreads());
  }

  // 步骤 1d：放大分支按 ROI 检测。
  // ROI 由基础分支的检出框规划（PlanDetailRois）；基础分支全无检出时
  // 没有任何依据可以裁剪，退回整图，与历史行为一致。
  if (!detail_branches.empty()) {
    PaddleOcrTimer t("pipeline.detail_phase");
    std::vector<DetectedBox> base_boxes;
    for (size_t bi = 0; bi < configs.size(); ++bi) {
      if (!slot_ok[bi] || slots[bi].detail) continue;
      base_boxes.insert(base_boxes.end(), slots[bi].boxes.begin(),
                        slots[bi].boxes.end());
    }
    std::vector<cv::Rect> rois =
        PlanDetailRois(base_boxes, image.cols, image.rows);
    // ROI 只在「确实能省面积」时才用：ROI 面积已接近整图时（小图、满屏
    // 文字），拆开只会多付几次推理的固定开销，还会因裁剪边界丢掉上下文。
    double roi_area = 0.0;
    for (const auto& r : rois) roi_area += static_cast<double>(r.area());
    const double image_area =
        static_cast<double>(image.cols) * static_cast<double>(image.rows);
    if (rois.empty() || roi_area > 0.9 * image_area) {
      rois.assign(1, cv::Rect(0, 0, image.cols, image.rows));
      roi_area = image_area;
    }
    OCR_LOG("RunPipeline: detail rois=%zu coverage=%.1f%%", rois.size(),
            100.0 * roi_area / std::max(1.0, image_area));
#if PADDLE_OCR_DEBUG
    for (const auto& r : rois) {
      OCR_LOG("  roi x=%d..%d y=%d..%d", r.x, r.x + r.width, r.y,
              r.y + r.height);
    }
#endif

    // 任务 = (放大分支 × ROI)。ROI 之间互不重叠，因此各任务的检出框
    // 天然不重复；结果先按任务下标收敛，再按「分支 → ROI 顺序」拼接。
    const size_t nroi = rois.size();
    const size_t ntasks = detail_branches.size() * nroi;
    std::vector<std::vector<DetectedBox>> task_boxes(ntasks);
    ParallelFor(ntasks, [&](size_t task) {
      const size_t bi = detail_branches[task / nroi];
      const cv::Rect& roi = rois[task % nroi];
      const double scale = slots[bi].scale;

      // ROI（原图坐标）→ 分支图坐标
      cv::Rect crop_rect(
          static_cast<int>(std::lround(roi.x * scale)),
          static_cast<int>(std::lround(roi.y * scale)),
          static_cast<int>(std::lround(roi.width * scale)),
          static_cast<int>(std::lround(roi.height * scale)));
      crop_rect &= cv::Rect(0, 0, slots[bi].image.cols, slots[bi].image.rows);
      if (crop_rect.width < 16 || crop_rect.height < 16) return;

      std::vector<float> input;
      int in_h = 0, in_w = 0;
      float ratio_h = 1.0f, ratio_w = 1.0f;
      if (!detector_.BuildDetectInput(slots[bi].image(crop_rect),
                                      configs[bi].det, input, in_h, in_w,
                                      ratio_h, ratio_w)) {
        return;
      }
      std::vector<float> out_data;
      int out_h = 0, out_w = 0;
      if (!detector_.RunDetectInference(input.data(), in_h, in_w, out_data,
                                        out_h, out_w)) {
        return;
      }
      auto boxes = detector_.PostProcess(out_data.data(), out_h, out_w, ratio_h,
                                         ratio_w, configs[bi].det,
                                         static_cast<int>(bi));
      // 坐标：ROI 内 → 分支图 → 原图
      for (auto& db : boxes) {
        for (auto& pt : db.points) {
          pt.x = static_cast<int>(
              std::lround((pt.x + crop_rect.x) / scale));
          pt.y = static_cast<int>(
              std::lround((pt.y + crop_rect.y) / scale));
        }
      }
      task_boxes[task] = std::move(boxes);
    }, BranchThreads());

    for (size_t task = 0; task < ntasks; ++task) {
      const size_t bi = detail_branches[task / nroi];
      slots[bi].boxes.insert(slots[bi].boxes.end(),
                             std::make_move_iterator(task_boxes[task].begin()),
                             std::make_move_iterator(task_boxes[task].end()));
    }
#if PADDLE_OCR_DEBUG
    for (size_t bi : detail_branches) {
      OCR_LOG("RunPipeline: branch[%zu '%s'] scale=%.2f detected %zu boxes "
              "in %zu rois",
              bi, configs[bi].name.c_str(), slots[bi].scale,
              slots[bi].boxes.size(), nroi);
    }
#endif
  }

  // 输入张量使命结束（识别阶段只用 8U 分支图），及时释放。
  for (auto& s : slots) {
    std::vector<float>().swap(s.input);
  }

  // 收敛：保持配置顺序（决定 branch_index 与 all_boxes 顺序）
  std::vector<BranchOutput> branches;
  branches.reserve(configs.size());
  std::vector<DetectedBox> all_boxes;
  for (size_t bi = 0; bi < configs.size(); ++bi) {
    if (!slot_ok[bi]) continue;
    all_boxes.insert(all_boxes.end(), slots[bi].boxes.begin(),
                     slots[bi].boxes.end());
    branches.push_back(std::move(slots[bi]));
  }

  if (all_boxes.empty()) {
    OCR_LOG("RunPipeline: no boxes from %zu branches", branches.size());
    return {};
  }

  // 步骤2~3.5：合并 + 同行拼接 + 识别窗构造（成员基底 + 双向外扩）。
  std::vector<MergedBox> regions;
  {
    PaddleOcrTimer t("pipeline.merge");
    regions = MergeAndBuildWindows(all_boxes, options.merge, image.cols,
                                   image.rows);
  }

  // 步骤4：择优识别（并行执行，见下方 ParallelFor）

  // 文本条增强三态对应的尝试列表：0 = 原样，1 = 增强。
  std::vector<int> enhance_passes;
  switch (options.rec.enhance_crops) {
    case 0: enhance_passes.push_back(0); break;
    case 1: enhance_passes.push_back(1); break;
    case 2:
    default:
      enhance_passes.push_back(0);
      enhance_passes.push_back(1);
      break;
  }

  // 步骤4：择优识别。
  //
  // 同一区域要在「原图 + 各分支图」×「原样 / 二次增强」上各识别一次。
  // 候选之间彼此独立，先拍平成一张任务表（只做几何运算，不做推理），再按
  // 「区域」分组并行：每个 worker 包干本区域的全部候选，区域的择优顺序与
  // 串行版完全一致（同分保留先出现者）。
  struct RecVariant {
    const cv::Mat* img = nullptr;
    double scale = 1.0;
    std::string label;
  };
  struct RecTask {
    size_t region = 0;              // 所属区域下标
    std::vector<cv::Point> box;     // 该变体上的裁剪框（已夹到图内）
    const cv::Mat* img = nullptr;
    int enhance = 0;
    std::string label;
    double scale = 1.0;
    double region_long = 0.0;
    double region_short = 0.0;
    // 增强候选的得分上界（见构建处的推导）。区域最优分一旦达到它，
    // 同区域所有「二次增强」候选数学上都不可能翻盘，直接跳过。
    double enhance_ub = std::numeric_limits<double>::infinity();
  };
  struct RecCandidate {
    std::string text;
    float confidence = 0.0f;
    double score = -std::numeric_limits<double>::infinity();
    std::string from;
  };

  std::vector<RecTask> tasks;
  tasks.reserve(regions.size() * enhance_passes.size() * 4);
  for (size_t ri = 0; ri < regions.size(); ++ri) {
    const MergedBox& region = regions[ri];
    if (region.points.size() < 3) continue;

    // region.points 已经是识别窗（成员基底 + 双向外扩，见
    // MergeAndBuildWindows），直接用于裁剪识别。
    const std::vector<cv::Point>& rec_window = region.points;
    // 打分几何用「识别窗」：窗长边是候选文本的真实可用宽度；短边用
    // 成员统计出的字高（union 短边被贴边碎片撑大后，expected_em 被
    // 低估，会奖励「读少了」的候选、漏掉读全的候选）。
    const double region_long = BoxLongSide(rec_window);
    const double region_short =
        region.text_height > 0.0 ? region.text_height
                                 : BoxShortSide(rec_window);

    // 增强候选（enhance=1）的得分上界。
    //
    // 打分公式（variant_select=4）：
    //   score = geometry + (raw_crop ? 2.0 : 0) + 0.001*scale
    //   geometry = 5 - penalty + 0.05*conf + 0.001*len
    //   penalty = |fill-1| 超出 10% 死区的部分（封顶 5.0）
    // 增强候选没有 raw 加成，且 geometry ≤ 5.05 + 0.001*len。取一个宽松的
    // 长度上界 len_ub（按「每字符至少 0.35 字高」反推，再乘 2 加 32），即可得
    // 增强类的上界；区域最优分一旦到顶，后续增强候选不必再跑。
    //
    // 未增强类（+2.0 raw 加成）的上界是不可达的：它要求 fill 恰好为 1、
    // conf 为 1 且长度顶到上界，实测永不触发，因此不做那一档的剪枝。
    // 退化区域（expected_em ≤ 0.5，竖排单字等）与 variant_select 0/1/2
    // 无此结构，上界取无穷大（行为与全量枚举一致）。
    const double expected_em = region_long / std::max(1.0, region_short);
    const double len_ub = expected_em / 0.35 * 2.0 + 32.0;
    const double scale_ub = 3.0;  // 最大分支放大倍数（upscale3x）
    const bool bounded_scoring =
        expected_em > 0.5 &&
        (options.rec.variant_select == 3 || options.rec.variant_select == 4);
    const double enhance_ub =
        bounded_scoring
            ? 5.05 + 0.001 * len_ub + 0.001 * scale_ub
            : std::numeric_limits<double>::infinity();

    // 候选变体：原图 + 各分支图。try_all_branches 时连没检出该区域的
    // 分支也试 —— 放大分支往往能提供更多笔画细节。
    std::vector<RecVariant> variants;
    variants.push_back({&image, 1.0, "raw"});
    for (const BranchOutput& b : branches) {
      // 二值化/白字提取类分支只负责「把框找全」，它们的像素不能喂识别器。
      if (!b.rec_source) continue;
      // region.branches 里存的是「配置下标」（DetectedBox::branch_index），
      // 与 branches 向量的位置可能因跳过空预处理分支而错位，用配置下标比。
      const bool contributed =
          std::find(region.branches.begin(), region.branches.end(),
                    b.config_index) != region.branches.end();
      if (!options.rec.try_all_branches && !contributed) continue;
      // raw 分支不缩放时与原图完全等价，不必重复跑。
      if (b.flag == kBranchRaw && std::abs(b.scale - 1.0) < 1e-9) continue;
      if (b.image.empty()) continue;
      variants.push_back(
          {&b.image, b.scale, b.name + (contributed ? "*" : "")});
    }

    for (const RecVariant& v : variants) {
      std::vector<cv::Point> box = ScaleBoxToBranch(rec_window, v.scale);
      // 夹到图内，避免边缘框在放大图上越界导致 warpPerspective 取空。
      for (auto& pt : box) {
        pt.x = std::max(0, std::min(v.img->cols - 1, pt.x));
        pt.y = std::max(0, std::min(v.img->rows - 1, pt.y));
      }
      for (int enhance : enhance_passes) {
        RecTask task;
        task.region = ri;
        task.box = box;
        task.img = v.img;
        task.enhance = enhance;
        task.label = v.label;
        task.scale = v.scale;
        task.region_long = region_long;
        task.region_short = region_short;
        task.enhance_ub = enhance_ub;
        tasks.push_back(std::move(task));
      }
    }
  }

  std::vector<RecCandidate> cands(tasks.size());
  {
    PaddleOcrTimer t("pipeline.recognize");
    // 并行度按「区域」切：一个 worker 包干本区域的全部候选。
    // 不能改成「按候选切」：ORT 会话的 intra-op 线程池只有几个线程，
    // 同时发起几十个 Run 会让每个 Run 都在池子里排队，实测单次调用墙钟
    // 从 45ms 涨到 180ms，整体反而更慢。
    ParallelFor(regions.size(), [&](size_t ri) {
      double best_score = -std::numeric_limits<double>::infinity();
      for (size_t ti = 0; ti < tasks.size(); ++ti) {
        if (tasks[ti].region != ri) continue;
        const RecTask& task = tasks[ti];
        // 提前收尾：区域最优分已达到增强类上界，剩下的增强候选翻不了盘。
        if (task.enhance != 0 && best_score >= task.enhance_ub) continue;
        RecognizeParams rp = options.rec;
        rp.enhance_crops = task.enhance;
        auto decoded = recognizer_.RecognizeRegion(*task.img, task.box, rp);
        if (decoded.first.empty()) continue;

        RecCandidate& c = cands[ti];
        c.text = std::move(decoded.first);
        c.confidence = decoded.second;
        c.score =
            VariantSelectionScore(rp.variant_select, c.text, c.confidence,
                                  task.region_long, task.region_short,
                                  /*raw_crop = */ task.enhance == 0) +
            // 平局破除：PP-OCR 的 CTC 置信度极易饱和（不同变体都是
            // 1.000），几何得分也可能完全相同。此时放大分支的候选拥有
            // 更多笔画细节，实测比 1x 原图更可靠（1x 把「舍」读成
            // 「合」，2x/3x 正确）。权重远小于几何差异，只做同分裁决。
            0.001 * task.scale;
        c.from = task.label + (task.enhance ? "+enh" : "");
        if (c.score > best_score) best_score = c.score;
        OCR_LOG("RunPipeline: region[%zu] variant='%s%s' em=%.2f/%.2f "
                "score=%.3f conf=%.3f '%s'",
                task.region, task.label.c_str(), task.enhance ? "+enh" : "",
                EstimateTextWidthInEm(c.text),
                task.region_long / std::max(1.0, task.region_short), c.score,
                c.confidence, c.text.c_str());
      }
    }, RegionThreads());
  }

  // 收敛：按「区域 → 变体 → 增强档」的原始顺序取最优（同分保留先出现者）
  std::vector<OcrBoxResult> out;
  out.reserve(regions.size());
  size_t next_task = 0;
  for (size_t ri = 0; ri < regions.size(); ++ri) {
    const RecCandidate* best = nullptr;
    for (; next_task < tasks.size() && tasks[next_task].region == ri;
         ++next_task) {
      const RecCandidate& c = cands[next_task];
      if (c.text.empty()) continue;
      if (best == nullptr || c.score > best->score) best = &c;
    }
    if (best == nullptr) continue;

    const MergedBox& region = regions[ri];
    if (options.verbose_stats) {
      OCR_LOG("RunPipeline: region[%zu] members=%d stitched=%d from='%s' -> "
              "'%s' (%.3f)",
              ri, region.member_count, region.stitched ? 1 : 0,
              best->from.c_str(), best->text.c_str(), best->confidence);
    }
    OcrBoxResult r;
    r.box = region.points;
    r.text = best->text;
    r.confidence = best->confidence;
    out.push_back(std::move(r));
  }

  OCR_LOG("RunPipeline: %zu branches, %zu boxes -> %zu regions -> %zu texts",
          branches.size(), all_boxes.size(), regions.size(), out.size());
  return out;
}

cv::Mat OcrEngine::DecodeImageBytes(const std::vector<uint8_t>& image_bytes) {
  if (image_bytes.empty()) return cv::Mat();
  // SCREENSHOT_CMAKE_PATCHED: copy buffer into cv::Mat (STL ABI workaround)
  cv::Mat buf_mat(1, static_cast<int>(image_bytes.size()), CV_8UC1);
  memcpy(buf_mat.data, image_bytes.data(), image_bytes.size());
  return cv::imdecode(buf_mat, cv::IMREAD_COLOR);
}

cv::Mat OcrEngine::RenderBranchPreview(const cv::Mat& image,
                                       uint32_t branch_flag,
                                       const OcrOptions& options) {
  if (image.empty()) return cv::Mat();
  OcrOptions opts = options;
  SanitizeOptions(&opts);
  const BranchConfig cfg = MakeBranchConfig(branch_flag, opts);
  return ApplyPreprocess(image, cfg.prep);
}


// 从图片字节数据识别文字
std::vector<OcrBoxResult> OcrEngine::RecognizeFromBytes(
    const std::vector<uint8_t>& image_bytes) {
  return RecognizeFromBytes(image_bytes, default_options_);
}

std::vector<OcrBoxResult> OcrEngine::RecognizeFromBytes(
    const std::vector<uint8_t>& image_bytes, const OcrOptions& options) {
  OCR_LOG("RecognizeFromBytes: input %zu bytes", image_bytes.size());
  cv::Mat image = DecodeImageBytes(image_bytes);
  if (image.empty()) {
    OCR_LOG("RecognizeFromBytes: cv::imdecode failed - image is empty");
    return {};
  }
  OCR_LOG("RecognizeFromBytes: decoded image %dx%d", image.cols, image.rows);
  return Recognize(image, options);
}

// 从图片文件路径识别文字
// 在 Windows 上，cv::imread 使用 C 运行时的 fopen()，只能处理 ANSI 编码路径。
// 非 ASCII 路径（如中文）会静默失败。
// 解决方案：通过宽字符 ifstream 读取文件字节，然后用 cv::imdecode 解码。
std::vector<OcrBoxResult> OcrEngine::RecognizeFromFile(
    const std::string& image_path) {
  return RecognizeFromFile(image_path, default_options_);
}

std::vector<OcrBoxResult> OcrEngine::RecognizeFromFile(
    const std::string& image_path, const OcrOptions& options) {
  OCR_LOG("RecognizeFromFile: path='%s' (hex: %s)",
          image_path.c_str(), HexDump(image_path).c_str());

  // 验证路径是否为有效的 UTF-8
  std::string path_err = ValidateUtf8Detailed(image_path);
  if (!path_err.empty()) {
    OCR_LOG("RecognizeFromFile: WARNING - image path has invalid UTF-8: %s",
            path_err.c_str());
    OCR_LOG("RecognizeFromFile: This may cause issues with cv::imread on Windows");
  }

#ifdef _WIN32
  // 在 Windows 上，cv::imread 使用 C 运行时 fopen()，只能处理
  // ANSI 代码页路径。非 ASCII 路径（如中文）会静默失败。
  // 解决方案：通过宽字符 ifstream 读取文件字节，然后用 cv::imdecode 解码。
  std::wstring wide_path = ToWString(image_path);
  std::ifstream file(std::filesystem::path(wide_path),
                     std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    OCR_LOG("RecognizeFromFile: failed to open file (wide path), errno=%d", errno);
    // 回退：直接尝试 cv::imread（适用于 ASCII 路径）
    OCR_LOG("RecognizeFromFile: trying cv::imread as fallback");
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      OCR_LOG("RecognizeFromFile: cv::imread fallback also failed - image is empty");
      return {};
    }
    OCR_LOG("RecognizeFromFile: cv::imread fallback loaded image %dx%d",
            image.cols, image.rows);
    return Recognize(image, options);
  }
#else
  // POSIX：路径按字节处理，UTF-8 原生支持。使用 std::ifstream 读取后
  // 交给 cv::imdecode，行为与 Windows 分支一致，也便于统一日志。
  std::ifstream file(std::filesystem::path(image_path),
                     std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    OCR_LOG("RecognizeFromFile: failed to open file, errno=%d", errno);
    return {};
  }
#endif

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  OCR_LOG("RecognizeFromFile: opened file, size=%lld bytes",
          static_cast<long long>(size));

  std::vector<uint8_t> buffer(static_cast<size_t>(size));
  if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
    OCR_LOG("RecognizeFromFile: failed to read file bytes");
    return {};
  }
  file.close();

  cv::Mat image = DecodeImageBytes(buffer);
  if (image.empty()) {
    OCR_LOG("RecognizeFromFile: cv::imdecode failed - image is empty "
            "(file may be corrupted or unsupported format)");
    return {};
  }
  OCR_LOG("RecognizeFromFile: decoded image %dx%d", image.cols, image.rows);
  return Recognize(image, options);
}

}  // namespace paddle_ocr
