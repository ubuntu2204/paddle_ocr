#include "ocr_engine.h"
#include "debug_utils.h"

#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>
#include <sstream>
#include <codecvt>
#include <locale>

namespace paddle_ocr {

// ============================================================================
// Utility: Validate and sanitize UTF-8 string
// Strips any invalid byte sequences to prevent Dart FormatException
// ============================================================================
static std::string SanitizeUtf8(const std::string& input) {
  std::string result;
  result.reserve(input.size());
  size_t i = 0;
  while (i < input.size()) {
    unsigned char c = static_cast<unsigned char>(input[i]);
    int seq_len = 0;
    if (c <= 0x7F) {
      seq_len = 1;
    } else if ((c & 0xE0) == 0xC0) {
      seq_len = 2;
    } else if ((c & 0xF0) == 0xE0) {
      seq_len = 3;
    } else if ((c & 0xF8) == 0xF0) {
      seq_len = 4;
    } else {
      // Invalid lead byte - skip
      i++;
      continue;
    }
    if (i + seq_len > input.size()) {
      // Truncated sequence - skip remaining
      break;
    }
    bool valid = true;
    for (int j = 1; j < seq_len; ++j) {
      if ((static_cast<unsigned char>(input[i + j]) & 0xC0) != 0x80) {
        valid = false;
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

// ============================================================================
// Utility: Convert std::string (UTF-8) to std::wstring (for Windows/ORT)
// ============================================================================
static std::wstring ToWString(const std::string& s) {
  if (s.empty()) return L"";
  int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring ws(len - 1, 0);
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
  return ws;
}

// ============================================================================
// Utility: GetRotatedCropImage - crop text region by 4-point polygon
// ============================================================================
static cv::Mat GetRotatedCropImage(const cv::Mat& image,
                                   const std::vector<cv::Point>& pts) {
  if (pts.size() != 4) return cv::Mat();

  // Reorder points to a canonical TL, TR, BR, BL order.
  // cv::minAreaRect().points() can return points in any rotation/order,
  // which causes width/height to be swapped.
  // Strategy: sort by y to get top-2 and bottom-2, then by x within each pair.
  std::vector<cv::Point> p = pts;
  std::sort(p.begin(), p.end(),
            [](const cv::Point& a, const cv::Point& b) {
              if (a.y != b.y) return a.y < b.y;
              return a.x < b.x;
            });
  // p[0], p[1] are the top-2 (smallest y); p[2], p[3] are the bottom-2.
  // Among top-2: smaller x = TL, larger x = TR
  // Among bottom-2: smaller x = BL, larger x = BR
  cv::Point tl, tr, bl, br;
  if (p[0].x <= p[1].x) { tl = p[0]; tr = p[1]; }
  else                  { tl = p[1]; tr = p[0]; }
  if (p[2].x <= p[3].x) { bl = p[2]; br = p[3]; }
  else                  { bl = p[3]; br = p[2]; }
  std::vector<cv::Point> ordered = {tl, tr, br, bl};

  // Calculate width (TL->TR) and height (TL->BL) from ordered points
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

  std::vector<cv::Point2f> src_pts = {
      cv::Point2f(static_cast<float>(ordered[0].x), static_cast<float>(ordered[0].y)),
      cv::Point2f(static_cast<float>(ordered[1].x), static_cast<float>(ordered[1].y)),
      cv::Point2f(static_cast<float>(ordered[2].x), static_cast<float>(ordered[2].y)),
      cv::Point2f(static_cast<float>(ordered[3].x), static_cast<float>(ordered[3].y))};

  std::vector<cv::Point2f> dst_pts = {
      cv::Point2f(0, 0),
      cv::Point2f(static_cast<float>(crop_w - 1), 0),
      cv::Point2f(static_cast<float>(crop_w - 1),
                  static_cast<float>(crop_h - 1)),
      cv::Point2f(0, static_cast<float>(crop_h - 1))};

  cv::Mat M = cv::getPerspectiveTransform(src_pts, dst_pts);
  cv::Mat crop_img;
  cv::warpPerspective(image, crop_img, M, cv::Size(crop_w, crop_h),
                      cv::BORDER_REPLICATE);
  return crop_img;
}

// ============================================================================
// TextDetector
// ============================================================================

TextDetector::TextDetector() : env_(ORT_LOGGING_LEVEL_WARNING, "OcrDet") {}
TextDetector::~TextDetector() = default;

bool TextDetector::Initialize(const std::string& model_path) {
  Ort::SessionOptions session_options;
  session_options.SetIntraOpNumThreads(4);
  session_options.SetGraphOptimizationLevel(
      GraphOptimizationLevel::ORT_ENABLE_ALL);

  std::wstring wide_path = ToWString(model_path);
  // Let Ort::Exception propagate so OcrEngine can capture the message
  session_ = Ort::Session(env_, wide_path.c_str(), session_options);
  Ort::AllocatorWithDefaultOptions alloc;
  auto in_name = session_.GetInputNameAllocated(0, alloc);
  auto out_name = session_.GetOutputNameAllocated(0, alloc);
  input_name_ = in_name.get();
  output_name_ = out_name.get();
  return true;
}

std::vector<std::vector<cv::Point>> TextDetector::Detect(
    const cv::Mat& image) {
  if (!session_) return {};

  float ratio_h = 1.0f, ratio_w = 1.0f;
  cv::Mat input_tensor_mat = Preprocess(image, ratio_h, ratio_w);
  if (input_tensor_mat.empty()) return {};

  int input_h = input_tensor_mat.rows;
  int input_w = input_tensor_mat.cols;
  OCR_LOG("Detect: input image %dx%d -> resized %dx%d",
          image.cols, image.rows, input_w, input_h);
  OCR_LOG("Detect: ratio_h=%.4f ratio_w=%.4f", ratio_h, ratio_w);

  // Convert HWC Mat to NCHW float vector
  std::vector<float> input_data(1 * 3 * input_h * input_w);
  std::vector<cv::Mat> bgr_channels(3);
  cv::split(input_tensor_mat, bgr_channels);
  for (int c = 0; c < 3; ++c) {
    std::memcpy(input_data.data() + c * input_h * input_w,
                bgr_channels[c].data,
                input_h * input_w * sizeof(float));
  }

  // Create input tensor
  std::array<int64_t, 4> input_shape = {1, 3, input_h, input_w};
  auto memory_info = Ort::MemoryInfo::CreateCpu(
      OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
      memory_info, input_data.data(), input_data.size(),
      input_shape.data(), input_shape.size());

  const char* in_names[] = {input_name_.c_str()};
  const char* out_names[] = {output_name_.c_str()};

  // Run inference
  try {
    auto output_tensors = session_.Run(
        Ort::RunOptions{nullptr}, in_names, &input_tensor, 1,
        out_names, 1);

    // Get output
    float* output_data = output_tensors[0].GetTensorMutableData<float>();
    auto output_shape = output_tensors[0].GetTensorTypeAndShapeInfo().GetShape();
    OCR_LOG("Detect: output shape=[%lld, %lld, %lld, %lld]",
            static_cast<long long>(output_shape[0]),
            static_cast<long long>(output_shape[1]),
            static_cast<long long>(output_shape[2]),
            static_cast<long long>(output_shape[3]));

    int out_h = static_cast<int>(output_shape[2]);
    int out_w = static_cast<int>(output_shape[3]);

    // Analyze output statistics
    int total = out_h * out_w;
    float min_val = output_data[0], max_val = output_data[0];
    double sum = 0.0;
    int above_thresh = 0;
    for (int i = 0; i < total; ++i) {
      float v = output_data[i];
      if (v < min_val) min_val = v;
      if (v > max_val) max_val = v;
      sum += v;
      if (v >= det_db_thresh_) above_thresh++;
    }
    double mean_val = sum / total;
    OCR_LOG("Detect: output stats: min=%.4f max=%.4f mean=%.4f",
            min_val, max_val, mean_val);
    OCR_LOG("Detect: pixels above thresh(%.3f): %d / %d (%.1f%%)",
            det_db_thresh_, above_thresh, total,
            100.0 * above_thresh / total);

    return PostProcess(output_data, out_h, out_w, ratio_h, ratio_w);
  } catch (const Ort::Exception& e) {
    OCR_LOG("Detect: ORT exception: %s", e.what());
    return {};
  } catch (const std::exception& e) {
    OCR_LOG("Detect: exception: %s", e.what());
    return {};
  }
}

cv::Mat TextDetector::Preprocess(const cv::Mat& image, float& ratio_h,
                                 float& ratio_w) {
  int h = image.rows;
  int w = image.cols;
  float ratio = 1.0f;

  // Resize so that max side <= max_side_len_
  if (h > max_side_len_ || w > max_side_len_) {
    if (h > w) {
      ratio = static_cast<float>(max_side_len_) / h;
    } else {
      ratio = static_cast<float>(max_side_len_) / w;
    }
  }

  int resize_h = std::max(static_cast<int>(std::round(h * ratio)), 1);
  int resize_w = std::max(static_cast<int>(std::round(w * ratio)), 1);

  // Pad to multiple of 32
  resize_h = std::max(resize_h, 32);
  resize_w = std::max(resize_w, 32);
  if (resize_h % 32 != 0) resize_h = (resize_h / 32 + 1) * 32;
  if (resize_w % 32 != 0) resize_w = (resize_w / 32 + 1) * 32;

  ratio_h = static_cast<float>(h) / resize_h;
  ratio_w = static_cast<float>(w) / resize_w;

  cv::Mat resized;
  cv::resize(image, resized, cv::Size(resize_w, resize_h));

  // Pad with zeros if needed
  if (resized.rows < resize_h || resized.cols < resize_w) {
    cv::Mat padded = cv::Mat::zeros(resize_h, resize_w, resized.type());
    resized.copyTo(padded(cv::Rect(0, 0, resized.cols, resized.rows)));
    resized = padded;
  }

  // Normalize with ImageNet mean/std, convert BGR->RGB
  cv::Mat float_img;
  resized.convertTo(float_img, CV_32FC3, 1.0 / 255.0);

  std::vector<cv::Mat> channels(3);
  cv::split(float_img, channels);
  // BGR -> RGB and normalize
  float mean_vals[] = {0.485f, 0.456f, 0.406f};  // R, G, B
  float std_vals[] = {0.229f, 0.224f, 0.225f};
  // channels: 0=B, 1=G, 2=R
  channels[2] = (channels[2] - mean_vals[0]) / std_vals[0];  // R
  channels[1] = (channels[1] - mean_vals[1]) / std_vals[1];  // G
  channels[0] = (channels[0] - mean_vals[2]) / std_vals[2];  // B

  cv::Mat normalized;
  cv::merge(channels, normalized);
  return normalized;
}

std::vector<std::vector<cv::Point>> TextDetector::PostProcess(
    const float* output, int out_h, int out_w,
    float ratio_h, float ratio_w) {
  // Convert output to cv::Mat
  cv::Mat pred(out_h, out_w, CV_32FC1, const_cast<float*>(output));

  // Threshold
  cv::Mat binary;
  cv::threshold(pred, binary, det_db_thresh_, 1.0, cv::THRESH_BINARY);
  binary.convertTo(binary, CV_8UC1);

  return BoxesFromBitmap(pred, binary, ratio_w, ratio_h);
}

std::vector<std::vector<cv::Point>> TextDetector::BoxesFromBitmap(
    const cv::Mat& pred, const cv::Mat& mask,
    float ratio_w, float ratio_h) {
  // Dilate mask slightly for better contour detection
  cv::Mat dilated;
  cv::Mat kernel = cv::getStructuringElement(
      cv::MORPH_RECT, cv::Size(2, 2));
  cv::dilate(mask, dilated, kernel);

  // Find contours
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(dilated, contours, cv::RETR_LIST,
                   cv::CHAIN_APPROX_SIMPLE);

  OCR_LOG("BoxesFromBitmap: found %zu contours", contours.size());

  std::vector<std::vector<cv::Point>> results;
  int skipped_size = 0, skipped_score = 0, skipped_unclip = 0;

  for (size_t ci = 0; ci < contours.size(); ++ci) {
    const auto& contour = contours[ci];
    if (contour.size() < 4) {
      OCR_LOG("BoxesFromBitmap: contour[%zu] skipped - only %zu points",
              ci, contour.size());
      continue;
    }

    // Get minimum area bounding rectangle
    cv::RotatedRect rect = cv::minAreaRect(contour);
    float short_side = std::min(rect.size.width, rect.size.height);
    if (short_side < 3.0f) {
      skipped_size++;
      OCR_LOG("BoxesFromBitmap: contour[%zu] skipped - short_side=%.1f < 3.0",
              ci, short_side);
      continue;
    }

    // Check box score
    std::vector<cv::Point> box_pts(4);
    cv::Point2f vertices[4];
    rect.points(vertices);
    for (int i = 0; i < 4; ++i) {
      box_pts[i] = cv::Point(static_cast<int>(vertices[i].x),
                              static_cast<int>(vertices[i].y));
    }

    float score = BoxScore(pred, box_pts);
    OCR_LOG("BoxesFromBitmap: contour[%zu] score=%.4f (thresh=%.3f)",
            ci, score, det_db_box_thresh_);
    if (score < det_db_box_thresh_) {
      skipped_score++;
      continue;
    }

    // Unclip (expand) the polygon
    std::vector<cv::Point> unclip_box;
    Unclip(box_pts, unclip_box, det_db_unclip_ratio_);
    if (unclip_box.empty()) {
      skipped_unclip++;
      continue;
    }

    // Get the final bounding rectangle from unclipped polygon
    cv::RotatedRect final_rect = cv::minAreaRect(unclip_box);
    if (std::min(final_rect.size.width, final_rect.size.height) < 5.0f) {
      OCR_LOG("BoxesFromBitmap: contour[%zu] final too small: %.1fx%.1f",
              ci, final_rect.size.width, final_rect.size.height);
      continue;
    }

    cv::Point2f final_vertices[4];
    final_rect.points(final_vertices);

    // Scale back to original image coordinates
    std::vector<cv::Point> result_box(4);
    for (int i = 0; i < 4; ++i) {
      result_box[i].x = static_cast<int>(
          std::round(final_vertices[i].x * ratio_w));
      result_box[i].y = static_cast<int>(
          std::round(final_vertices[i].y * ratio_h));
    }
    results.push_back(result_box);
  }

  OCR_LOG("BoxesFromBitmap: results: %zu (skipped: size=%d score=%d unclip=%d)",
          results.size(), skipped_size, skipped_score, skipped_unclip);

  // Sort boxes by y-coordinate (top to bottom), then x (left to right)
  std::sort(results.begin(), results.end(),
            [](const std::vector<cv::Point>& a,
               const std::vector<cv::Point>& b) {
              int min_ya = std::min({a[0].y, a[1].y, a[2].y, a[3].y});
              int min_yb = std::min({b[0].y, b[1].y, b[2].y, b[3].y});
              if (min_ya != min_yb) return min_ya < min_yb;
              int min_xa = std::min({a[0].x, a[1].x, a[2].x, a[3].x});
              int min_xb = std::min({b[0].x, b[1].x, b[2].x, b[3].x});
              return min_xa < min_xb;
            });

  return results;
}

float TextDetector::BoxScore(const cv::Mat& pred,
                             const std::vector<cv::Point>& box) {
  // Use polygon mask to compute mean score (PaddleOCR "slow" mode).
  // This is more accurate than bounding-rect mean because it excludes
  // background pixels outside the text polygon.
  int xmin = std::max(0, std::min({box[0].x, box[1].x, box[2].x, box[3].x}));
  int xmax = std::min(pred.cols - 1,
                      std::max({box[0].x, box[1].x, box[2].x, box[3].x}));
  int ymin = std::max(0, std::min({box[0].y, box[1].y, box[2].y, box[3].y}));
  int ymax = std::min(pred.rows - 1,
                      std::max({box[0].y, box[1].y, box[2].y, box[3].y}));

  if (xmin >= xmax || ymin >= ymax) return 0.0f;

  // Crop the region of interest
  cv::Mat roi = pred(cv::Rect(xmin, ymin, xmax - xmin + 1, ymax - ymin + 1));

  // Build polygon mask shifted to ROI coordinates
  std::vector<cv::Point> shifted_box;
  shifted_box.reserve(box.size());
  for (const auto& pt : box) {
    shifted_box.emplace_back(pt.x - xmin, pt.y - ymin);
  }

  cv::Mat mask = cv::Mat::zeros(roi.size(), CV_8UC1);
  cv::fillPoly(mask, shifted_box, cv::Scalar(1));

  // Compute mean only over masked pixels
  return static_cast<float>(cv::mean(roi, mask)[0]);
}

void TextDetector::Unclip(const std::vector<cv::Point>& in_poly,
                          std::vector<cv::Point>& out_poly,
                          float unclip_ratio) {
  // Compute polygon area and perimeter using the shoelace formula.
  // Keep the SIGNED area to determine winding order.
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

  float distance = area * unclip_ratio / perimeter;

  // Determine outward normal sign based on winding order.
  // In image coordinates (y down), shoelace area > 0 means clockwise.
  // For clockwise polygons, the outward normal is to the RIGHT of the
  // edge direction: (dy, -dx).  For counterclockwise it is to the LEFT:
  // (-dy, dx).
  float normal_sign = (signed_area > 0) ? 1.0f : -1.0f;

  // Expand each vertex outward along the average of its two adjacent
  // edge normals (mitered offset).
  std::vector<cv::Point2f> expanded(n);
  for (int i = 0; i < n; ++i) {
    int prev = (i - 1 + n) % n;
    int next = (i + 1) % n;

    // Edge vectors (normalized)
    float e1x = static_cast<float>(in_poly[i].x - in_poly[prev].x);
    float e1y = static_cast<float>(in_poly[i].y - in_poly[prev].y);
    float e2x = static_cast<float>(in_poly[next].x - in_poly[i].x);
    float e2y = static_cast<float>(in_poly[next].y - in_poly[i].y);

    float len1 = std::sqrt(e1x * e1x + e1y * e1y);
    float len2 = std::sqrt(e2x * e2x + e2y * e2y);
    if (len1 > 0) { e1x /= len1; e1y /= len1; }
    if (len2 > 0) { e2x /= len2; e2y /= len2; }

    // Outward normals — direction depends on winding order.
    // Right normal: (dy, -dx), Left normal: (-dy, dx)
    float n1x = normal_sign * e1y;
    float n1y = normal_sign * (-e1x);
    float n2x = normal_sign * e2y;
    float n2y = normal_sign * (-e2x);

    // Average normal (mitered)
    float avg_nx = n1x + n2x;
    float avg_ny = n1y + n2y;
    float avg_len = std::sqrt(avg_nx * avg_nx + avg_ny * avg_ny);
    if (avg_len > 0) { avg_nx /= avg_len; avg_ny /= avg_len; }

    expanded[i].x = static_cast<float>(in_poly[i].x) + avg_nx * distance;
    expanded[i].y = static_cast<float>(in_poly[i].y) + avg_ny * distance;
  }

  // Verify the expansion actually increased the area; if not, the normal
  // direction was wrong — flip it (safety net).
  float new_signed_area = 0.0f;
  for (int i = 0; i < n; ++i) {
    int j = (i + 1) % n;
    new_signed_area += expanded[i].x * expanded[j].y -
                       expanded[j].x * expanded[i].y;
  }
  float new_area = std::abs(new_signed_area) / 2.0f;
  if (new_area < area) {
    // Expansion went the wrong way — flip normals and redo.
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

  out_poly.resize(n);
  for (int i = 0; i < n; ++i) {
    out_poly[i].x = static_cast<int>(std::round(expanded[i].x));
    out_poly[i].y = static_cast<int>(std::round(expanded[i].y));
  }
}

// ============================================================================
// TextRecognizer
// ============================================================================

TextRecognizer::TextRecognizer()
    : env_(ORT_LOGGING_LEVEL_WARNING, "OcrRec") {}
TextRecognizer::~TextRecognizer() = default;

bool TextRecognizer::Initialize(const std::string& model_path,
                                const std::string& dict_path) {
  Ort::SessionOptions session_options;
  session_options.SetIntraOpNumThreads(4);
  session_options.SetGraphOptimizationLevel(
      GraphOptimizationLevel::ORT_ENABLE_ALL);

  std::wstring wide_path = ToWString(model_path);
  // Let Ort::Exception propagate so OcrEngine can capture the message
  session_ = Ort::Session(env_, wide_path.c_str(), session_options);
  Ort::AllocatorWithDefaultOptions alloc;
  auto in_name = session_.GetInputNameAllocated(0, alloc);
  auto out_name = session_.GetOutputNameAllocated(0, alloc);
  input_name_ = in_name.get();
  output_name_ = out_name.get();

  return LoadDict(dict_path);
}

bool TextRecognizer::LoadDict(const std::string& dict_path) {
  OCR_LOG("LoadDict: path='%s'", dict_path.c_str());

  // Clear any previously loaded dict (supports re-initialization)
  dict_.clear();

  // Open in binary mode to detect encoding / BOM
  std::ifstream file(dict_path, std::ios::in | std::ios::binary);
  if (!file.is_open()) {
    OCR_LOG("LoadDict: FAILED to open file: %s", dict_path.c_str());
    fprintf(stderr, "Failed to open dict file: %s\n", dict_path.c_str());
    return false;
  }

  // Read entire file into a buffer for encoding analysis
  std::string file_content((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
  file.close();

  OCR_LOG("LoadDict: file size=%zu bytes", file_content.size());

  // Check for BOM
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

  // Dump first 64 bytes for encoding verification
  OCR_LOG("LoadDict: first 64 bytes hex: %s",
          HexDump(file_content.substr(0, 64)).c_str());

  // Validate that the file is valid UTF-8
  std::string utf8_err = ValidateUtf8Detailed(file_content);
  if (!utf8_err.empty()) {
    OCR_LOG("LoadDict: WARNING - file is NOT valid UTF-8: %s", utf8_err.c_str());
    fprintf(stderr, "Dict file UTF-8 validation error: %s\n", utf8_err.c_str());
    // Continue anyway - SanitizeUtf8 will clean individual entries
  } else {
    OCR_LOG("LoadDict: file is valid UTF-8");
  }

  // Parse lines from the in-memory buffer
  std::istringstream stream(file_content);
  std::string line;
  int line_num = 0;
  while (std::getline(stream, line)) {
    line_num++;
    // Remove trailing \r if present (Windows line endings)
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    // Validate each dict entry is valid UTF-8
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

  // Log first 5 and last 5 entries for verification
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

std::vector<std::pair<std::string, float>> TextRecognizer::Recognize(
    const cv::Mat& image,
    const std::vector<std::vector<cv::Point>>& boxes) {
  if (!session_ || dict_.empty()) return {};

  std::vector<std::pair<std::string, float>> results;

  for (size_t bi = 0; bi < boxes.size(); ++bi) {
    const auto& box = boxes[bi];
    cv::Mat crop = PreprocessCrop(image, box);
    if (crop.empty()) {
      results.push_back({"", 0.0f});
      continue;
    }

    int crop_h = crop.rows;
    int crop_w = crop.cols;

    // Convert to NCHW float tensor
    std::vector<float> input_data(1 * 3 * crop_h * crop_w);
    std::vector<cv::Mat> channels(3);
    cv::split(crop, channels);
    for (int c = 0; c < 3; ++c) {
      std::memcpy(input_data.data() + c * crop_h * crop_w,
                  channels[c].data, crop_h * crop_w * sizeof(float));
    }

    std::array<int64_t, 4> input_shape = {1, 3, crop_h, crop_w};
    OCR_LOG("Recognize: box[%zu] input tensor shape=[1,3,%d,%d]",
            bi, crop_h, crop_w);
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

      // Output shape: [batch, seq_len, num_classes] (3D)
      //         or:   [seq_len, num_classes] (2D, batch squeezed)
      // NOTE: out_shape[0] is batch (usually 1), NOT seq_len!
      int seq_len, num_classes;
      if (out_shape.size() == 3) {
        seq_len = static_cast<int>(out_shape[1]);      // [1, seq_len, classes]
        num_classes = static_cast<int>(out_shape[2]);
      } else if (out_shape.size() == 2) {
        seq_len = static_cast<int>(out_shape[0]);      // [seq_len, classes]
        num_classes = static_cast<int>(out_shape[1]);
      } else {
        OCR_LOG("Recognize: box[%zu] unexpected output dim=%zu",
                bi, out_shape.size());
        results.push_back({"", 0.0f});
        continue;
      }
      OCR_LOG("Recognize: box[%zu] output shape=[%lld,%lld,%lld] -> "
              "seq_len=%d num_classes=%d",
              bi,
              static_cast<long long>(out_shape[0]),
              out_shape.size() >= 2 ? static_cast<long long>(out_shape[1]) : 0LL,
              out_shape.size() >= 3 ? static_cast<long long>(out_shape[2]) : 0LL,
              seq_len, num_classes);

      auto decoded = DecodeOutput(output_data, seq_len, num_classes);
      results.push_back(decoded);
    } catch (const Ort::Exception& e) {
      fprintf(stderr, "Recognition inference failed: %s\n", e.what());
      results.push_back({"", 0.0f});
    }
  }

  return results;
}

cv::Mat TextRecognizer::PreprocessCrop(
    const cv::Mat& image, const std::vector<cv::Point>& box) {
  cv::Mat crop = GetRotatedCropImage(image, box);
  if (crop.empty()) return crop;

  OCR_LOG("PreprocessCrop: crop size %dx%d", crop.cols, crop.rows);

  // Resize to fixed height, keep aspect ratio
  int h = crop.rows;
  int w = crop.cols;
  float ratio = static_cast<float>(rec_img_h_) / h;
  int new_w = std::max(static_cast<int>(w * ratio), 1);

  OCR_LOG("PreprocessCrop: resize to %dx%d (ratio=%.4f)",
          new_w, rec_img_h_, ratio);

  cv::Mat resized;
  cv::resize(crop, resized, cv::Size(new_w, rec_img_h_));

  // Pad width to at least 32
  int pad_w = std::max(new_w, 32);
  if (new_w < pad_w) {
    cv::Mat padded = cv::Mat::zeros(rec_img_h_, pad_w, resized.type());
    resized.copyTo(padded(cv::Rect(0, 0, new_w, rec_img_h_)));
    resized = padded;
  }

  // Normalize: pixel/255, then (x - 0.5) / 0.5
  cv::Mat float_img;
  resized.convertTo(float_img, CV_32FC3, 1.0 / 255.0);
  float_img = (float_img - 0.5f) / 0.5f;

  OCR_LOG("PreprocessCrop: final tensor %dx%d", float_img.cols, float_img.rows);
  return float_img;
}

std::pair<std::string, float> TextRecognizer::DecodeOutput(
    const float* data, int seq_len, int num_classes) {
  // CTC greedy decoding
  std::string text;
  float total_conf = 0.0f;
  int valid_count = 0;
  int last_idx = 0;  // CTC blank = 0

  OCR_LOG("DecodeOutput: seq_len=%d, num_classes=%d, dict_size=%zu",
          seq_len, num_classes, dict_.size());

  for (int t = 0; t < seq_len; ++t) {
    const float* row = data + t * num_classes;

    // Find argmax
    int max_idx = 0;
    float max_val = row[0];
    for (int c = 1; c < num_classes; ++c) {
      if (row[c] > max_val) {
        max_val = row[c];
        max_idx = c;
      }
    }

    // CTC: skip blank (0) and repeated characters
    if (max_idx != 0 && max_idx != last_idx) {
      int dict_idx = max_idx - 1;  // dict starts at index 0, model output at 1
      if (dict_idx >= 0 && dict_idx < static_cast<int>(dict_.size())) {
        text += dict_[dict_idx];
        // Softmax confidence
        float sum_exp = 0.0f;
        for (int c = 0; c < num_classes; ++c) {
          sum_exp += std::exp(row[c]);
        }
        float conf = std::exp(row[max_idx]) / sum_exp;
        total_conf += conf;
        valid_count++;
      } else {
        OCR_LOG("DecodeOutput: WARNING - dict_idx %d out of range [0, %zu)",
                dict_idx, dict_.size());
      }
    }
    last_idx = max_idx;
  }

  float avg_conf = valid_count > 0 ? total_conf / valid_count : 0.0f;

  // Debug: log raw text before sanitization
  OCR_LOG("DecodeOutput: raw text length=%zu bytes, conf=%.4f, valid_count=%d",
          text.size(), avg_conf, valid_count);
  OCR_LOG("DecodeOutput: raw text hex: %s", HexDump(text).c_str());

  // Validate raw text
  std::string raw_err = ValidateUtf8Detailed(text);
  if (!raw_err.empty()) {
    OCR_LOG("DecodeOutput: WARNING - raw text has invalid UTF-8: %s", raw_err.c_str());
  }

  std::string sanitized = SanitizeUtf8(text);

  // Validate sanitized text
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
// OcrEngine
// ============================================================================

OcrEngine::OcrEngine() = default;
OcrEngine::~OcrEngine() = default;

bool OcrEngine::Initialize(const std::string& det_model_path,
                           const std::string& rec_model_path,
                           const std::string& dict_path) {
  last_error_.clear();
  OCR_LOG("Initialize: det_model='%s'", det_model_path.c_str());
  OCR_LOG("Initialize: rec_model='%s'", rec_model_path.c_str());
  OCR_LOG("Initialize: dict_path='%s'", dict_path.c_str());

  // Validate paths are valid UTF-8
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

std::vector<OcrBoxResult> OcrEngine::Recognize(const cv::Mat& image) {
  if (!initialized_ || image.empty()) {
    OCR_LOG("Recognize: skipped (initialized=%d, image_empty=%d)",
            initialized_, image.empty());
    return {};
  }

  OCR_LOG("Recognize: image %dx%d, channels=%d",
          image.cols, image.rows, image.channels());

  // Detect text regions
  auto boxes = detector_.Detect(image);
  OCR_LOG("Recognize: detected %zu text regions", boxes.size());
  if (boxes.empty()) return {};

  // Recognize text in each region
  auto texts = recognizer_.Recognize(image, boxes);
  OCR_LOG("Recognize: recognized %zu text results", texts.size());

  // Combine results
  std::vector<OcrBoxResult> results;
  size_t count = std::min(boxes.size(), texts.size());
  for (size_t i = 0; i < count; ++i) {
    OcrBoxResult r;
    r.box = boxes[i];
    r.text = texts[i].first;
    r.confidence = texts[i].second;

    // Validate each result text before sending to Flutter
    std::string err = ValidateUtf8Detailed(r.text);
    if (!err.empty()) {
      OCR_LOG("Recognize: ERROR - result[%zu] text has invalid UTF-8: %s", i, err.c_str());
      OCR_LOG("Recognize: result[%zu] hex: %s", i, HexDump(r.text).c_str());
      // Sanitize again as a safety net
      r.text = SanitizeUtf8(r.text);
    }

    OCR_LOG("Recognize: result[%zu] box=[(%d,%d)(%d,%d)(%d,%d)(%d,%d)] "
            "conf=%.4f text_hex='%s'",
            i,
            r.box[0].x, r.box[0].y, r.box[1].x, r.box[1].y,
            r.box[2].x, r.box[2].y, r.box[3].x, r.box[3].y,
            r.confidence, HexDump(r.text).c_str());

    results.push_back(std::move(r));
  }
  return results;
}

std::vector<OcrBoxResult> OcrEngine::RecognizeFromBytes(
    const std::vector<uint8_t>& image_bytes) {
  OCR_LOG("RecognizeFromBytes: input %zu bytes", image_bytes.size());
  cv::Mat image = cv::imdecode(image_bytes, cv::IMREAD_COLOR);
  if (image.empty()) {
    OCR_LOG("RecognizeFromBytes: cv::imdecode failed - image is empty");
    return {};
  }
  OCR_LOG("RecognizeFromBytes: decoded image %dx%d", image.cols, image.rows);
  return Recognize(image);
}

std::vector<OcrBoxResult> OcrEngine::RecognizeFromFile(
    const std::string& image_path) {
  OCR_LOG("RecognizeFromFile: path='%s' (hex: %s)",
          image_path.c_str(), HexDump(image_path).c_str());

  // Validate that the path is valid UTF-8
  std::string path_err = ValidateUtf8Detailed(image_path);
  if (!path_err.empty()) {
    OCR_LOG("RecognizeFromFile: WARNING - image path has invalid UTF-8: %s",
            path_err.c_str());
    OCR_LOG("RecognizeFromFile: This may cause issues with cv::imread on Windows");
  }

  // On Windows, cv::imread uses C runtime fopen() which only handles
  // ANSI codepage paths. Non-ASCII paths (e.g. Chinese chars) fail silently.
  // Workaround: read file bytes via wide-char ifstream, then cv::imdecode.
  std::wstring wide_path = ToWString(image_path);
  std::ifstream file(wide_path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    OCR_LOG("RecognizeFromFile: failed to open file (wide path), errno=%d", errno);
    // Fallback: try cv::imread directly (works for ASCII paths)
    OCR_LOG("RecognizeFromFile: trying cv::imread as fallback");
    cv::Mat image = cv::imread(image_path, cv::IMREAD_COLOR);
    if (image.empty()) {
      OCR_LOG("RecognizeFromFile: cv::imread fallback also failed - image is empty");
      return {};
    }
    OCR_LOG("RecognizeFromFile: cv::imread fallback loaded image %dx%d",
            image.cols, image.rows);
    return Recognize(image);
  }

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

  cv::Mat image = cv::imdecode(buffer, cv::IMREAD_COLOR);
  if (image.empty()) {
    OCR_LOG("RecognizeFromFile: cv::imdecode failed - image is empty "
            "(file may be corrupted or unsupported format)");
    return {};
  }
  OCR_LOG("RecognizeFromFile: decoded image %dx%d", image.cols, image.rows);
  return Recognize(image);
}

}  // namespace paddle_ocr
