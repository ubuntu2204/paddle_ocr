#include "ocr_engine.h"
#include "debug_utils.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
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
  session_options.SetIntraOpNumThreads(4);  // 使用4线程进行推理
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

  float ratio_h = 1.0f, ratio_w = 1.0f;
  cv::Mat input_tensor_mat = Preprocess(image, params, ratio_h, ratio_w);
  if (input_tensor_mat.empty()) return no_boxes;

  int input_h = input_tensor_mat.rows;
  int input_w = input_tensor_mat.cols;
  OCR_LOG("Detect[b%d]: input image %dx%d -> resized %dx%d",
          branch_index, image.cols, image.rows, input_w, input_h);
  OCR_LOG("Detect[b%d]: ratio_h=%.4f ratio_w=%.4f thresh=%.3f box_thresh=%.3f",
          branch_index, ratio_h, ratio_w, static_cast<float>(params.thresh),
          static_cast<float>(params.box_thresh));

  // 将 HWC 格式的 Mat 转换为 NCHW 格式的浮点向量
  std::vector<float> input_data(1 * 3 * input_h * input_w);
  cv::Mat bgr_channels[3]; // SCREENSHOT_CMAKE_PATCHED: avoid STL ABI mismatch
  cv::split(input_tensor_mat, bgr_channels);
  for (int c = 0; c < 3; ++c) {
    std::memcpy(input_data.data() + c * input_h * input_w,
                bgr_channels[c].data,
                input_h * input_w * sizeof(float));
  }

  // 创建输入张量
  std::array<int64_t, 4> input_shape = {1, 3, input_h, input_w};
  auto memory_info = Ort::MemoryInfo::CreateCpu(
      OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
      memory_info, input_data.data(), input_data.size(),
      input_shape.data(), input_shape.size());

  const char* in_names[] = {input_name_.c_str()};
  const char* out_names[] = {output_name_.c_str()};

  // 执行推理
  try {
    auto output_tensors = session_.Run(
        Ort::RunOptions{nullptr}, in_names, &input_tensor, 1,
        out_names, 1);

    // 获取输出数据
    float* output_data = output_tensors[0].GetTensorMutableData<float>();
    auto output_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();

    int out_h = static_cast<int>(output_shape[2]);
    int out_w = static_cast<int>(output_shape[3]);

    // 分析输出统计信息
    int total = out_h * out_w;
    float min_val = output_data[0], max_val = output_data[0];
    double sum = 0.0;
    int above_thresh = 0;
    for (int i = 0; i < total; ++i) {
      float v = output_data[i];
      if (v < min_val) min_val = v;
      if (v > max_val) max_val = v;
      sum += v;
      if (v >= params.thresh) above_thresh++;
    }
    double mean_val = sum / total;
    OCR_LOG("Detect[b%d]: output stats: min=%.4f max=%.4f mean=%.4f",
            branch_index, min_val, max_val, mean_val);
    OCR_LOG("Detect[b%d]: pixels above thresh(%.3f): %d / %d (%.1f%%)",
            branch_index, static_cast<float>(params.thresh), above_thresh, total,
            100.0 * above_thresh / total);

    return PostProcess(output_data, out_h, out_w, ratio_h, ratio_w, params,
                       branch_index);
  } catch (const Ort::Exception& e) {
    OCR_LOG("Detect[b%d]: ORT exception: %s", branch_index, e.what());
    return no_boxes;
  } catch (const std::exception& e) {
    OCR_LOG("Detect[b%d]: exception: %s", branch_index, e.what());
    return no_boxes;
  }
}

// 预处理：缩放、填充、归一化
cv::Mat TextDetector::Preprocess(const cv::Mat& image,
                                 const DetectParams& params, float& ratio_h,
                                 float& ratio_w) {
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

  // 如需则用零填充
  if (resized.rows < resize_h || resized.cols < resize_w) {
    cv::Mat padded = cv::Mat::zeros(resize_h, resize_w, resized.type());
    resized.copyTo(padded(cv::Rect(0, 0, resized.cols, resized.rows)));
    resized = padded;
  }

  // 归一化：使用 ImageNet 均值/标准差，BGR->RGB
  cv::Mat float_img;
  resized.convertTo(float_img, CV_32FC3, 1.0 / 255.0);

  cv::Mat channels[3]; // SCREENSHOT_CMAKE_PATCHED: avoid STL ABI mismatch
  cv::split(float_img, channels);
  // BGR -> RGB 并归一化
  float mean_vals[] = {0.485f, 0.456f, 0.406f};  // R, G, B
  float std_vals[] = {0.229f, 0.224f, 0.225f};
  // channels: 0=B, 1=G, 2=R
  channels[2] = (channels[2] - mean_vals[0]) / std_vals[0];  // R
  channels[1] = (channels[1] - mean_vals[1]) / std_vals[1];  // G
  channels[0] = (channels[0] - mean_vals[2]) / std_vals[2];  // B

  cv::Mat normalized;
  cv::merge(channels, 3, normalized); // SCREENSHOT_CMAKE_PATCHED: use pointer overload
  return normalized;
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
  session_options.SetIntraOpNumThreads(4);
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
  std::vector<std::pair<std::string, float>> results;
  if (!session_ || dict_.empty()) return results;

  results.reserve(boxes.size());
  for (size_t bi = 0; bi < boxes.size(); ++bi) {
    if (boxes[bi].size() < 3) {
      results.push_back({"", 0.0f});
      continue;
    }
    results.push_back(RecognizeRegion(image, boxes[bi], params));
  }
  return results;
}

// 识别单个文本区域（一张图一个区域一次推理）
std::pair<std::string, float> TextRecognizer::RecognizeRegion(
    const cv::Mat& image, const std::vector<cv::Point>& box,
    const RecognizeParams& params) {
  if (!session_ || dict_.empty()) return {"", 0.0f};

  cv::Mat crop = PreprocessCrop(image, box, params);
  if (crop.empty()) return {"", 0.0f};

  int crop_h = crop.rows;
  int crop_w = crop.cols;

  // 转换为 NCHW 浮点张量
  std::vector<float> input_data(1 * 3 * crop_h * crop_w);
  cv::Mat channels[3]; // SCREENSHOT_CMAKE_PATCHED: avoid STL ABI mismatch
  cv::split(crop, channels);
  for (int c = 0; c < 3; ++c) {
    std::memcpy(input_data.data() + c * crop_h * crop_w,
                channels[c].data, crop_h * crop_w * sizeof(float));
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
    auto output_tensors = session_.Run(
        Ort::RunOptions{nullptr}, in_names, &input_tensor, 1,
        out_names, 1);

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

  // 宽度至少填充到 32
  int pad_w = std::max(new_w, 32);
  if (new_w < pad_w) {
    cv::Mat padded = cv::Mat::zeros(img_h, pad_w, resized.type());
    resized.copyTo(padded(cv::Rect(0, 0, new_w, img_h)));
    resized = padded;
  }

  // 归一化：pixel/255，然后 (x - 0.5) / 0.5
  cv::Mat float_img;
  resized.convertTo(float_img, CV_32FC3, 1.0 / 255.0);
  float_img = (float_img - 0.5f) / 0.5f;

  OCR_LOG("PreprocessCrop: final tensor %dx%d", float_img.cols, float_img.rows);
  return float_img;
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
      kBranchRaw,       kBranchAdaptiveBin, kBranchOtsuBin,
      kBranchClahe,     kBranchUpscale2x,   kBranchSensitive,
      kBranchSharpen,   kBranchDenoiseClahe, kBranchWhiteText,
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
// 多分支完整管道
//
//   1. 每一路预处理 → 检测 → 把框换算回原图坐标
//   2. 跨分支去重合并（同一区域只留一个框，但记住是哪几路贡献的）
//   3. 同行碎框拼接（把一行被切碎的字合成整行）
//   4. 择优识别：同一个区域在「原图 + 各贡献分支图」上各识别一次，
//      取「字符数 × 置信度」最高的结果
// ===========================================================================
std::vector<OcrBoxResult> OcrEngine::RunPipeline(const cv::Mat& image,
                                                 const OcrOptions& options) {
  const std::vector<BranchConfig> configs = BuildBranchConfigs(options);
  std::vector<BranchOutput> branches;
  branches.reserve(configs.size());

  std::vector<DetectedBox> all_boxes;
  for (size_t bi = 0; bi < configs.size(); ++bi) {
    const BranchConfig& cfg = configs[bi];

    BranchOutput out;
    out.flag = cfg.flag;
    out.name = cfg.name;
    out.rec_source = cfg.rec_source;

    double scale = 1.0;
    out.image = ApplyPreprocess(image, cfg.prep, &scale);
    out.scale = scale;
    if (out.image.empty()) {
      OCR_LOG("RunPipeline: branch[%zu '%s'] preprocess produced empty image",
              bi, cfg.name.c_str());
      continue;
    }

    out.boxes = detector_.DetectScored(out.image, cfg.det, static_cast<int>(bi));

    // 框坐标从分支图换算回原图
    if (std::abs(scale - 1.0) > 1e-9) {
      for (auto& db : out.boxes) {
        for (auto& pt : db.points) {
          pt.x = static_cast<int>(std::round(pt.x / scale));
          pt.y = static_cast<int>(std::round(pt.y / scale));
        }
      }
    }

    OCR_LOG("RunPipeline: branch[%zu '%s'] scale=%.2f detected %zu boxes", bi,
            cfg.name.c_str(), scale, out.boxes.size());
    all_boxes.insert(all_boxes.end(), out.boxes.begin(), out.boxes.end());
    branches.push_back(std::move(out));
  }

  if (all_boxes.empty()) {
    OCR_LOG("RunPipeline: no boxes from %zu branches", branches.size());
    return {};
  }

  // 步骤2：跨分支合并
  std::vector<MergedBox> regions = MergeDetectedBoxes(all_boxes, options.merge);

  // 步骤3：同行拼接
  regions = StitchSameLine(regions, options.merge);

  // 步骤3.5：区域外扩，把贴边笔画纳回裁剪区
  PadRegions(&regions, options.merge.region_pad_ratio, image.cols, image.rows);

  // 排序 + 截断
  SortBoxesForReadingOrder(&regions, options.merge.sort_order);
  if (options.merge.max_regions > 0 &&
      static_cast<int>(regions.size()) > options.merge.max_regions) {
    regions.resize(static_cast<size_t>(options.merge.max_regions));
  }

  // 步骤4：择优识别
  std::vector<OcrBoxResult> results;
  results.reserve(regions.size());

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

  for (size_t ri = 0; ri < regions.size(); ++ri) {
    const MergedBox& region = regions[ri];
    if (region.points.size() < 3) continue;

    std::string best_text;
    float best_conf = 0.0f;
    double best_score = -std::numeric_limits<double>::infinity();
    std::string best_from;
    // 打分用「外扩前」的参考几何：外扩后的短边含空白，会低估预期字数。
    const double region_long = region.ref_long_side > 0.0
                                   ? region.ref_long_side
                                   : BoxLongSide(region.points);
    const double region_short = region.ref_short_side > 0.0
                                    ? region.ref_short_side
                                    : BoxShortSide(region.points);

    // 候选变体：原图 + 各分支图。try_all_branches 时连没检出该区域的
    // 分支也试 —— 放大分支往往能提供更多笔画细节。
    struct Variant {
      const cv::Mat* img;
      double scale;
      std::string label;
    };
    std::vector<Variant> variants;
    variants.push_back({&image, 1.0, "raw"});
    for (size_t bi = 0; bi < branches.size(); ++bi) {
      const BranchOutput& b = branches[bi];
      // 二值化/白字提取类分支只负责「把框找全」，它们的像素不能喂识别器。
      if (!b.rec_source) continue;
      const bool contributed =
          std::find(region.branches.begin(), region.branches.end(),
                    static_cast<int>(bi)) != region.branches.end();
      if (!options.rec.try_all_branches && !contributed) continue;
      // raw 分支不缩放时与原图完全等价，不必重复跑。
      if (b.flag == kBranchRaw && std::abs(b.scale - 1.0) < 1e-9) continue;
      if (b.image.empty()) continue;
      variants.push_back({&b.image, b.scale,
                          b.name + (contributed ? "*" : "")});
    }

    for (const Variant& variant : variants) {
      const cv::Mat& src = *variant.img;
      std::vector<cv::Point> box = ScaleBoxToBranch(region.points, variant.scale);
      // 夹到图内，避免边缘框在放大图上越界导致 warpPerspective 取空。
      for (auto& pt : box) {
        pt.x = std::max(0, std::min(src.cols - 1, pt.x));
        pt.y = std::max(0, std::min(src.rows - 1, pt.y));
      }

      for (int enhance : enhance_passes) {
        RecognizeParams rp = options.rec;
        rp.enhance_crops = enhance;
        auto decoded = recognizer_.RecognizeRegion(src, box, rp);
        if (decoded.first.empty()) continue;

        const double score =
            VariantSelectionScore(rp.variant_select, decoded.first,
                                  decoded.second, region_long, region_short,
                                  /*raw_crop = */ enhance == 0);
        if (score > best_score) {
          best_score = score;
          best_text = std::move(decoded.first);
          best_conf = decoded.second;
          best_from = variant.label + (enhance ? "+enh" : "");
        }
      }
    }

    if (best_text.empty()) continue;

    if (options.verbose_stats) {
      OCR_LOG("RunPipeline: region[%zu] variants=%zu members=%d stitched=%d "
              "from='%s' -> '%s' (%.3f)",
              ri, variants.size(), region.member_count, region.stitched ? 1 : 0,
              best_from.c_str(), best_text.c_str(), best_conf);
    }

    OcrBoxResult r;
    r.box = region.points;
    r.text = std::move(best_text);
    r.confidence = best_conf;
    results.push_back(std::move(r));
  }

  OCR_LOG("RunPipeline: %zu branches, %zu boxes -> %zu regions -> %zu texts",
          branches.size(), all_boxes.size(), regions.size(), results.size());
  return results;
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
