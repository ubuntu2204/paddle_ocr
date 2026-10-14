#include "ocr_preprocess.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

#include "debug_utils.h"
#include "opencv_abi_utils.h"

namespace paddle_ocr {

namespace {

// 把 BGR 图转成单通道灰度；已经是单通道则直接复制。
cv::Mat ToGray(const cv::Mat& src) {
  if (src.channels() == 1) return src.clone();
  cv::Mat gray;
  if (src.channels() == 4) {
    cv::cvtColor(src, gray, cv::COLOR_BGRA2GRAY);
  } else {
    cv::cvtColor(src, gray, cv::COLOR_BGR2GRAY);
  }
  return gray;
}

// 把单通道图还原成 3 通道（检测/识别模型固定吃 3 通道输入）。
cv::Mat GrayToBgr(const cv::Mat& gray) {
  cv::Mat out;
  cv::cvtColor(gray, out, cv::COLOR_GRAY2BGR);
  return out;
}

// 自适应直方图均衡，只作用于 Lab 的 L 通道，避免把色偏放大。
cv::Mat ApplyClahe(const cv::Mat& src, double clip_limit, int grid) {
  cv::Mat lab;
  if (src.channels() == 3) {
    cv::cvtColor(src, lab, cv::COLOR_BGR2Lab);
  } else {
    lab = src.clone();  // 单通道本身就相当于亮度图
  }
  std::vector<cv::Mat> ch;
  cv::split(lab, ch);
  cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(
      clip_limit, cv::Size(grid > 0 ? grid : 8, grid > 0 ? grid : 8));
  clahe->apply(ch[0], ch[0]);
  cv::merge(ch, lab);
  if (src.channels() == 3) {
    cv::Mat out;
    cv::cvtColor(lab, out, cv::COLOR_Lab2BGR);
    return out;
  }
  return ch[0];
}

// 非锐化掩模：src + amount * (src - blur(src))。
cv::Mat ApplyUnsharp(const cv::Mat& src, double amount) {
  if (amount <= 0.0) return src.clone();
  cv::Mat blur;
  cv::GaussianBlur(src, blur, cv::Size(0, 0), 2.0);
  cv::Mat out;
  cv::addWeighted(src, 1.0 + amount, blur, -amount, 0.0, out);
  return out;
}

// 白色叠加字幕提取。
//
// 监控/直播画面的字幕几乎总是「纯白字 + 半透明底」，而它压着的景物
// （树丛、广告牌、路面）大多是高饱和的。用 V - S 作为响应图，白色文字
// 得到强正响应、彩色景物被压制，比直接用灰度图的分隔度高一个量级。
cv::Mat ApplyWhiteTextExtract(const cv::Mat& src, const PreprocessParams& p) {
  cv::Mat bgr;
  if (src.channels() == 3) {
    bgr = src;
  } else if (src.channels() == 1) {
    cv::cvtColor(src, bgr, cv::COLOR_GRAY2BGR);
  } else {
    cv::cvtColor(src, bgr, cv::COLOR_BGRA2BGR);
  }

  cv::Mat hsv;
  cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
  std::vector<cv::Mat> ch;
  cv::split(hsv, ch);
  // ch[1] = S，ch[2] = V。饱和减法是饱和运算，负值直接落到 0。
  cv::Mat resp;
  cv::subtract(ch[2], ch[1], resp);

  // 响应图再拉一次对比度，让淡字与背景彻底分开。
  resp = ApplyClahe(resp, p.clahe_clip_limit > 0.5 ? p.clahe_clip_limit : 3.0,
                    p.clahe_grid_size > 0 ? p.clahe_grid_size : 8);
  return GrayToBgr(resp);
}

// 形态学结构元：核半径 k → (2k+1) x (2k+1) 矩形。
cv::Mat MakeKernel(int radius) {
  int size = std::max(1, radius * 2 + 1);
  return cv::getStructuringElement(cv::MORPH_RECT, cv::Size(size, size));
}

}  // namespace

// ===========================================================================
// 预处理插件接口实现
//
// 内置 Transform 类是对下方自由算子的薄封装（保持算子可独立调用）；
// PreprocessPipeline 负责有序执行与坐标缩放统计。
// ===========================================================================

namespace {

// shared_ptr 单例工厂：内置步骤无状态，全局共享一份即可。
template <typename T>
std::shared_ptr<IImageTransform> SharedStep() {
  static const std::shared_ptr<IImageTransform> step = std::make_shared<T>();
  return step;
}

}  // namespace

cv::Mat ScaleTransform::Apply(const cv::Mat& src, const PreprocessParams& p) const {
  return ApplyScale(src, p.scale_percent);
}

cv::Mat EnhanceTransform::Apply(const cv::Mat& src, const PreprocessParams& p) const {
  return ApplyEnhance(src, p);
}

cv::Mat BinarizeTransform::Apply(const cv::Mat& src, const PreprocessParams& p) const {
  return ApplyBinarize(src, p);
}

cv::Mat MorphologyTransform::Apply(const cv::Mat& src, const PreprocessParams& p) const {
  return ApplyMorphology(src, p);
}

cv::Mat ColorTransform::Apply(const cv::Mat& src, const PreprocessParams& p) const {
  return ApplyColorMode(src, p.color_mode);
}

PreprocessPipeline& PreprocessPipeline::AddStep(
    std::shared_ptr<IImageTransform> step) {
  if (step) steps_.push_back(std::move(step));
  return *this;
}

void PreprocessPipeline::UseDefaults() {
  Clear();
  steps_ = {
      SharedStep<ScaleTransform>(),     SharedStep<EnhanceTransform>(),
      SharedStep<BinarizeTransform>(),  SharedStep<MorphologyTransform>(),
      SharedStep<ColorTransform>(),
  };
}

void PreprocessPipeline::Clear() { steps_.clear(); }

std::vector<std::string> PreprocessPipeline::StepNames() const {
  std::vector<std::string> names;
  names.reserve(steps_.size());
  for (const auto& s : steps_) names.push_back(s->name());
  return names;
}

cv::Mat PreprocessPipeline::Run(const cv::Mat& src, const PreprocessParams& p,
                                double* applied_scale) const {
  if (applied_scale) *applied_scale = 1.0;
  if (src.empty()) return src.clone();

  // applied_scale 的语义沿袭历史 ApplyPreprocess：报告管线里的缩放系数。
  // 只有当第一步是内置缩放步骤时才统计（自定义管线请自行换算坐标）。
  const bool first_is_scale =
      !steps_.empty() && std::string(steps_.front()->name()) == "scale";
  if (applied_scale && first_is_scale) {
    *applied_scale = (p.scale_percent > 0 ? p.scale_percent : 100) / 100.0;
  }

  cv::Mat out = src;
  for (const auto& step : steps_) {
    out = step->Apply(out, p);
  }
  return out;
}

const PreprocessPipeline& DefaultPreprocessPipeline() {
  static const PreprocessPipeline pipeline = [] {
    PreprocessPipeline p;
    p.UseDefaults();
    return p;
  }();
  return pipeline;
}

// ===========================================================================
// 单步算子
// ===========================================================================

cv::Mat ApplyScale(const cv::Mat& src, int scale_percent,
                   double* applied_scale) {
  const double scale = (scale_percent > 0 ? scale_percent : 100) / 100.0;
  if (applied_scale) *applied_scale = scale;
  if (std::abs(scale - 1.0) < 1e-6 || src.empty()) return src.clone();

  int w = std::max(1, static_cast<int>(std::lround(src.cols * scale)));
  int h = std::max(1, static_cast<int>(std::lround(src.rows * scale)));
  cv::Mat out;
  // 放大用立方插值保边，缩小用区域插值抑制摩尔纹与锯齿。
  cv::resize(src, out, cv::Size(w, h), 0, 0,
             scale > 1.0 ? cv::INTER_CUBIC : cv::INTER_AREA);
  return out;
}

cv::Mat ApplyEnhance(const cv::Mat& src, const PreprocessParams& p) {
  if (src.empty()) return src.clone();
  switch (p.enhance_mode) {
    case kEnhanceNone:
      return src.clone();
    case kEnhanceClahe:
      return ApplyClahe(src, p.clahe_clip_limit, p.clahe_grid_size);
    case kEnhanceSharpen:
      return ApplyUnsharp(src, p.sharpen_amount);
    case kEnhanceClaheSharpen:
      return ApplyUnsharp(
          ApplyClahe(src, p.clahe_clip_limit, p.clahe_grid_size),
          p.sharpen_amount);
    case kEnhanceDenoise: {
      cv::Mat denoised;
      // d=9 在 8UC3 上足够抹掉块噪，同时不会糊掉笔画边缘。
      cv::bilateralFilter(src, denoised, 9, 75, 75);
      return denoised;
    }
    case kEnhanceDenoiseClahe: {
      cv::Mat denoised;
      cv::bilateralFilter(src, denoised, 9, 75, 75);
      return ApplyClahe(denoised, p.clahe_clip_limit, p.clahe_grid_size);
    }
    case kEnhanceWhiteText:
      return ApplyWhiteTextExtract(src, p);
    default:
      return src.clone();
  }
}

bool DetectTextIsBright(const cv::Mat& gray) {
  if (gray.empty()) return true;
  cv::Mat blur;
  cv::GaussianBlur(gray, blur, cv::Size(0, 0), 5.0);

  // 高通响应：文字笔画相对其局部背景要么明显更亮、要么明显更暗，
  // 统计正负两侧的总能量即可判断极性。
  cv::Mat diff;
  cv::subtract(gray, blur, diff, cv::noArray(), CV_32S);

  double positive = 0.0;
  double negative = 0.0;
  const int rows = diff.rows;
  const size_t step = diff.step1();
  for (int y = 0; y < rows; ++y) {
    const int32_t* row = diff.ptr<int32_t>(y);
    for (int x = 0; x < diff.cols; ++x) {
      const int32_t v = row[x];
      if (v > 0) {
        positive += v;
      } else {
        negative -= v;
      }
    }
  }
  (void)step;
  return positive >= negative;
}

cv::Mat ApplyBinarize(const cv::Mat& src, const PreprocessParams& p) {
  if (src.empty() || p.binarize_mode == kBinarizeNone) return src.clone();

  const cv::Mat gray = ToGray(src);
  const bool bright_text = DetectTextIsBright(gray);

  // 邻域大小：显式给奇数就用，否则按短边尺寸自动取（约 1.5 倍字高）。
  int block = p.adaptive_block_size;
  if (block <= 0) {
    block = std::max(9, static_cast<int>(std::min(gray.rows, gray.cols) / 12));
    if (block % 2 == 0) block += 1;
  } else if (block % 2 == 0) {
    block += 1;
  }
  const double c = p.adaptive_c;

  cv::Mat bin;
  switch (p.binarize_mode) {
    case kBinarizeOtsuInv:
      cv::threshold(gray, bin, 0, 255,
                    cv::THRESH_BINARY_INV | cv::THRESH_OTSU);
      break;
    case kBinarizeOtsuPlain:
      cv::threshold(gray, bin, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
      break;
    case kBinarizeAdaptiveGaussInv:
      cv::adaptiveThreshold(gray, bin, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                            cv::THRESH_BINARY_INV, block, c);
      break;
    case kBinarizeAdaptiveMeanInv:
      cv::adaptiveThreshold(gray, bin, 255, cv::ADAPTIVE_THRESH_MEAN_C,
                            cv::THRESH_BINARY_INV, block, c);
      break;
    case kBinarizeAuto: {
      // 自动：全局 Otsu 只在「光照均匀」时可靠；只要极性判断显示文字偏暗
      // （典型文档扫描），就用正色阈值把文字压成黑字白底。
      if (bright_text) {
        cv::adaptiveThreshold(gray, bin, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                              cv::THRESH_BINARY_INV, block, c);
      } else {
        cv::threshold(gray, bin, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
      }
      break;
    }
    default:
      return src.clone();
  }
  return GrayToBgr(bin);
}

cv::Mat ApplyMorphology(const cv::Mat& src, const PreprocessParams& p) {
  if (src.empty()) return src.clone();
  cv::Mat out = src;
  // 先开后闭：二值化后文字是黑字白底，开运算（腐→膨）让黑色扩张，
  // 既清掉白色小噪点又能把断裂笔画连上；随后的闭运算再把剩下的
  // 黑色碎点去掉。两者都为 0 时本函数是空操作。
  if (p.morph_open_kernel > 0) {
    cv::morphologyEx(out, out, cv::MORPH_OPEN, MakeKernel(p.morph_open_kernel));
  }
  if (p.morph_close_kernel > 0) {
    cv::morphologyEx(out, out, cv::MORPH_CLOSE, MakeKernel(p.morph_close_kernel));
  }
  return out;
}

cv::Mat ApplyColorMode(const cv::Mat& src, ColorMode mode) {
  if (src.empty() || mode == kColorKeep) return src;
  if (mode == kColorGray3) return GrayToBgr(ToGray(src));
  return src;
}

cv::Mat ApplyPreprocess(const cv::Mat& src, const PreprocessParams& p,
                        double* applied_scale) {
  return DefaultPreprocessPipeline().Run(src, p, applied_scale);
}

cv::Mat PrepareCropForRecognition(const cv::Mat& crop,
                                  const PreprocessParams& params) {
  if (crop.empty()) return crop;
  PreprocessParams p = params;
  // 文本条只有几十像素高，整图那套邻域（31~41）会把笔画当成背景消掉。
  // 这里按条的实际高度重取：约 0.9 倍字高，夹在 [9, 31]。
  const int h = std::max(8, crop.rows);
  int block = p.adaptive_block_size;
  const int auto_block = std::max(9, std::min(31, static_cast<int>(h * 0.9)));
  if (block <= 0 || block > h * 2) block = auto_block;
  if (block % 2 == 0) block += 1;
  p.adaptive_block_size = block;

  if (p.morph_close_kernel * 2 + 1 > h) p.morph_close_kernel = 0;
  if (p.morph_open_kernel * 2 + 1 > h) p.morph_open_kernel = 0;
  p.scale_percent = 100;  // 缩放交给识别阶段统一处理
  return ApplyPreprocess(crop, p);
}

// ===========================================================================
// 文本框几何
// ===========================================================================

double BoxArea(const std::vector<cv::Point>& box) {
  if (box.size() < 3) return 0.0;
  return cv::contourArea(PointsToMat(box));
}

cv::Rect BoxBoundingRect(const std::vector<cv::Point>& box) {
  if (box.empty()) return cv::Rect();
  return cv::boundingRect(PointsToMat(box));
}

cv::Point2f BoxCenter(const std::vector<cv::Point>& box) {
  if (box.empty()) return cv::Point2f(0, 0);
  double sx = 0, sy = 0;
  for (const auto& pt : box) {
    sx += pt.x;
    sy += pt.y;
  }
  return cv::Point2f(static_cast<float>(sx / box.size()),
                     static_cast<float>(sy / box.size()));
}

namespace {
// 取框的最小旋转外接矩形（点集不足 3 个时退化为包围盒）。
cv::RotatedRect ToRotatedRect(const std::vector<cv::Point>& box) {
  if (box.size() < 3) {
    cv::Rect r = BoxBoundingRect(box);
    return cv::RotatedRect(cv::Point2f(r.tl()), cv::Size2f(1.f, 1.f), 0.f);
  }
  return cv::minAreaRect(PointsToMat(box));
}

// 两个框的「垂直咬合比例」：y 范围重叠 / 较短者。处于同一文本行的
// 多路检出互相咬合很深（≥0.5），而贴边碎片（图像顶部的残框等）与
// 字带几乎没有垂直交集。
double BandOverlapRatio(const std::vector<cv::Point>& a,
                        const std::vector<cv::Point>& b) {
  const cv::Rect ra = BoxBoundingRect(a);
  const cv::Rect rb = BoxBoundingRect(b);
  const double overlap_y =
      std::min(ra.y + ra.height, rb.y + rb.height) - std::max(ra.y, rb.y);
  if (overlap_y <= 0) return 0.0;
  return overlap_y / std::min(ra.height, rb.height);
}

// 中位数（副本排序，n=0 返回 fallback）。
double MedianOf(std::vector<double> v, double fallback) {
  if (v.empty()) return fallback;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n % 2 == 1 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) * 0.5;
}

// 成员框短边的中位数 = 「真实字高」。union 的短边会被贴边碎片撑大，
// 用它反推预期字数会系统性低估；真实字高才是择优打分的可靠标尺。
double MemberTextHeight(const std::vector<std::vector<cv::Point>>& members,
                        double fallback) {
  std::vector<double> shorts;
  for (const auto& m : members) {
    if (m.size() < 3) continue;
    const cv::RotatedRect r = cv::minAreaRect(PointsToMat(m));
    shorts.push_back(std::min(r.size.width, r.size.height));
  }
  return MedianOf(std::move(shorts), fallback);
}
}  // namespace

double BoxShortSide(const std::vector<cv::Point>& box) {
  if (box.empty()) return 0.0;
  const cv::RotatedRect r = ToRotatedRect(box);
  return std::min(r.size.width, r.size.height);
}

double BoxLongSide(const std::vector<cv::Point>& box) {
  if (box.empty()) return 0.0;
  const cv::RotatedRect r = ToRotatedRect(box);
  return std::max(r.size.width, r.size.height);
}

double BoxAngle(const std::vector<cv::Point>& box) {
  if (box.size() < 3) return 0.0;
  // 直接从最长边的向量算倾角，不依赖 RotatedRect::points 的重载差异。
  double long_angle = 0.0;
  double best_len = -1.0;
  for (size_t i = 0; i < box.size(); ++i) {
    const cv::Point& a = box[i];
    const cv::Point& b = box[(i + 1) % box.size()];
    const double len = std::hypot(b.x - a.x, b.y - a.y);
    if (len > best_len) {
      best_len = len;
      long_angle = std::abs(std::atan2(b.y - a.y, b.x - a.x)) * 180.0 / CV_PI;
    }
  }
  if (long_angle > 90.0) long_angle = 180.0 - long_angle;
  return long_angle;
}

double BoxIntersectionArea(const std::vector<cv::Point>& a,
                           const std::vector<cv::Point>& b) {
  if (a.size() < 3 || b.size() < 3) return 0.0;
  cv::RotatedRect ra = ToRotatedRect(a);
  cv::RotatedRect rb = ToRotatedRect(b);
  // 退化矩形（宽或高为 0）没有面积可言。
  if (ra.size.width <= 0 || ra.size.height <= 0 || rb.size.width <= 0 ||
      rb.size.height <= 0) {
    return 0.0;
  }
  cv::Mat intersect;
  const int code = cv::rotatedRectangleIntersection(ra, rb, intersect);
  if (code == cv::INTERSECT_NONE || intersect.empty()) return 0.0;
  return cv::contourArea(intersect);
}

double BoxIoU(const std::vector<cv::Point>& a,
              const std::vector<cv::Point>& b) {
  const double inter = BoxIntersectionArea(a, b);
  if (inter <= 0.0) return 0.0;
  const double area_a = std::max(1.0, BoxArea(a));
  const double area_b = std::max(1.0, BoxArea(b));
  const double uni = area_a + area_b - inter;
  return uni > 0.0 ? inter / uni : 0.0;
}

double BoxContainment(const std::vector<cv::Point>& inner,
                      const std::vector<cv::Point>& outer) {
  const double area_inner = BoxArea(inner);
  if (area_inner <= 0.0) return 0.0;
  return BoxIntersectionArea(inner, outer) / area_inner;
}

std::vector<cv::Point> UnionBoxes(
    const std::vector<std::vector<cv::Point>>& boxes) {
  std::vector<cv::Point> all_pts;
  for (const auto& b : boxes) {
    all_pts.insert(all_pts.end(), b.begin(), b.end());
  }
  if (all_pts.size() < 3) return all_pts;
  cv::RotatedRect r = cv::minAreaRect(PointsToMat(all_pts));
  cv::Point2f verts[4];
  r.points(verts);
  return std::vector<cv::Point>(verts, verts + 4);
}

bool BoxesAreDuplicate(const std::vector<cv::Point>& a,
                       const std::vector<cv::Point>& b, const MergeParams& m) {
  const double iou = BoxIoU(a, b);
  if (iou >= m.merge_iou_threshold) return true;
  // 包含度取两个方向的最大值：碎框被整行框包住、或大框几乎覆盖小框，
  // 都算重复。
  const double contain =
      std::max(BoxContainment(a, b), BoxContainment(b, a));
  if (contain >= m.merge_contain_threshold) return true;

  // 水平带判据：文本行是水平延伸目标，不同分支对同一行的垂直定位可能
  // 差得很远（贴边碎片 y 从 0 开始），IoU 与包含度都够不着。
  // 「垂直咬合 ≥ 50% 且水平重叠 > 70% 短宽」的两框只可能是同一行：
  // 上下紧邻的两行字即便算上 0.3 倍字高的外扩，垂直咬合也 < 50%。
  const cv::Rect ra = BoxBoundingRect(a);
  const cv::Rect rb = BoxBoundingRect(b);
  if (ra.width < 1 || rb.width < 1) return false;

  const double overlap_x =
      std::min(ra.x + ra.width, rb.x + rb.width) - std::max(ra.x, rb.x);
  if (overlap_x <= 0) return false;
  if (overlap_x / std::min(ra.width, rb.width) <= 0.7) return false;
  return BandOverlapRatio(a, b) >= 0.5;
}

// ===========================================================================
// 多分支结果合并
// ===========================================================================

namespace {

// 一个合并中的簇。成员框全部保留，外接形状按需重算。
struct Cluster {
  std::vector<std::vector<cv::Point>> members;
  std::set<int> branches;
  double score = 0.0;
  std::vector<cv::Point> shape;  // 缓存的 union 形状

  void Add(const DetectedBox& db) {
    members.push_back(db.points);
    branches.insert(db.branch_index);
    score = std::max(score, db.score);
    shape.clear();
  }
  void Absorb(const Cluster& other) {
    members.insert(members.end(), other.members.begin(), other.members.end());
    branches.insert(other.branches.begin(), other.branches.end());
    score = std::max(score, other.score);
    shape.clear();
  }
  const std::vector<cv::Point>& Shape() {
    if (shape.empty() && !members.empty()) shape = UnionBoxes(members);
    return shape;
  }
};

}  // namespace

std::vector<MergedBox> MergeDetectedBoxes(
    const std::vector<DetectedBox>& boxes, const MergeParams& m) {
  // 面积大的先入簇，让它当代表；后续小框只会并进去，不会反过来吞掉大框。
  std::vector<size_t> order(boxes.size());
  for (size_t i = 0; i < boxes.size(); ++i) order[i] = i;
  std::stable_sort(order.begin(), order.end(), [&](size_t lhs, size_t rhs) {
    return BoxArea(boxes[lhs].points) > BoxArea(boxes[rhs].points);
  });

  std::vector<Cluster> clusters;
  clusters.reserve(boxes.size());

  for (size_t idx : order) {
    const DetectedBox& db = boxes[idx];
    if (db.points.size() < 3) continue;

    int hit = -1;
    for (size_t ci = 0; ci < clusters.size(); ++ci) {
      if (BoxesAreDuplicate(db.points, clusters[ci].Shape(), m)) {
        hit = static_cast<int>(ci);
        break;
      }
    }
    if (hit < 0) {
      clusters.emplace_back();
      clusters.back().Add(db);
      continue;
    }

    clusters[hit].Add(db);
    // 并入后簇的形状变大，可能又与别的簇重叠 —— 立刻做一次簇间归并，
    // 否则最终会输出多个高度重叠的重复区域。
    // 倒序遍历，删除元素不会影响尚未访问的下标。
    for (int ci = static_cast<int>(clusters.size()) - 1; ci >= 0; --ci) {
      if (ci == hit) continue;
      if (BoxesAreDuplicate(clusters[hit].Shape(), clusters[ci].Shape(), m)) {
        clusters[hit].Absorb(clusters[ci]);
        clusters.erase(clusters.begin() + ci);
        if (ci < hit) --hit;  // hit 随前面的元素一起前移
      }
    }
  }

  std::vector<MergedBox> out;
  out.reserve(clusters.size());
  for (auto& c : clusters) {
    MergedBox mb;
    mb.points = c.Shape();
    mb.member_boxes = c.members;
    mb.branches.assign(c.branches.begin(), c.branches.end());
    mb.score = c.score;
    mb.member_count = static_cast<int>(c.members.size());
    mb.stitched = false;
    mb.ref_long_side = BoxLongSide(mb.points);
    mb.ref_short_side = BoxShortSide(mb.points);
    mb.text_height = MemberTextHeight(mb.member_boxes, mb.ref_short_side);
    if (mb.points.size() >= 3) {
      OCR_LOG("MergeDetectedBoxes: cluster rect=(%d,%d %dx%d) members=%d",
              BoxBoundingRect(mb.points).x, BoxBoundingRect(mb.points).y,
              BoxBoundingRect(mb.points).width,
              BoxBoundingRect(mb.points).height, mb.member_count);
      for (const auto& m : mb.member_boxes) {
        const cv::Rect r = BoxBoundingRect(m);
        OCR_LOG("  member: (%d,%d %dx%d)", r.x, r.y, r.width, r.height);
      }
      out.push_back(std::move(mb));
    }
  }

  if (m.max_regions > 0 &&
      static_cast<int>(out.size()) > m.max_regions) {
    std::stable_sort(out.begin(), out.end(),
                     [](const MergedBox& a, const MergedBox& b) {
                       return BoxArea(a.points) > BoxArea(b.points);
                     });
    out.resize(static_cast<size_t>(m.max_regions));
  }
  OCR_LOG("MergeDetectedBoxes: %zu boxes -> %zu regions (branches: %s)",
          boxes.size(), out.size(), BranchFlagsName(m.branches).c_str());
  return out;
}

std::vector<MergedBox> StitchSameLine(const std::vector<MergedBox>& boxes,
                                      const MergeParams& m) {
  if (!m.stitch_same_line || boxes.size() < 2) return boxes;

  struct Item {
    cv::Rect rect;
    double height;
    double angle;
    const MergedBox* src;
  };
  std::vector<Item> items;
  items.reserve(boxes.size());
  for (size_t i = 0; i < boxes.size(); ++i) {
    const auto& b = boxes[i];
    if (b.points.size() < 3) continue;
    Item it;
    it.rect = BoxBoundingRect(b.points);
    it.height = std::max(1.0, BoxShortSide(b.points));
    it.angle = BoxAngle(b.points);
    it.src = &b;
    items.push_back(it);
  }

  // 只拼接近水平的行；竖排、斜排文字拼接会串行。
  std::vector<char> used(items.size(), 0);
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.rect.y != b.rect.y) return a.rect.y < b.rect.y;
    return a.rect.x < b.rect.x;
  });

  std::vector<MergedBox> out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (used[i]) continue;
    used[i] = 1;

    // line_members 全部是 items 的下标。
    std::vector<size_t> line_members;
    std::vector<std::vector<cv::Point>> shapes;
    line_members.push_back(i);
    shapes.push_back(items[i].src->points);

    double base_height = std::max(1.0, items[i].height);
    const int line_left = items[i].rect.x;
    int line_right = items[i].rect.x + items[i].rect.width;
    const bool eligible = items[i].angle <= 30.0;

    if (eligible) {
      // 从左往右链式延伸：每轮只接上「最近的一个合法邻居」。
      while (line_members.size() < 32) {  // 防御：单行不接纳无限串联
        int best = -1;
        double best_gap = std::numeric_limits<double>::max();
        for (size_t j = 0; j < items.size(); ++j) {
          if (used[j]) continue;
          const Item& cand = items[j];
          if (cand.angle > 30.0) continue;

          // 只向右延伸，且不接已被当前行完整覆盖的框。少了这两条，
          // 同一基线上的另一段独立字幕会被逐跳吞进来。
          if (cand.rect.x < line_left) continue;
          if (cand.rect.x + cand.rect.width <= line_right) continue;

          // 垂直重叠必须足够，且高度不能差太多。
          const int y_top = std::max(items[i].rect.y, cand.rect.y);
          const int y_bot = std::min(items[i].rect.y + items[i].rect.height,
                                     cand.rect.y + cand.rect.height);
          const double y_overlap =
              static_cast<double>(y_bot - y_top) /
              std::max(1.0, static_cast<double>(
                                std::min(items[i].rect.height,
                                         cand.rect.height)));
          if (y_overlap < m.line_min_y_overlap) continue;

          const double h_diff = std::abs(cand.height - base_height) /
                                std::max(base_height, cand.height);
          if (h_diff > m.line_max_height_diff_ratio) continue;

          // gap 可以为负（碎框互相压边），但不能大过候选自身宽度。
          const int gap = cand.rect.x - line_right;
          if (-gap > cand.rect.width) continue;
          const double gap_ratio =
              static_cast<double>(gap) / std::max(base_height, cand.height);
          if (gap_ratio > m.line_max_gap_ratio) continue;
          if (gap_ratio < best_gap) {
            best_gap = gap_ratio;
            best = static_cast<int>(j);
          }
        }
        if (best < 0) break;
        used[best] = 1;
        line_members.push_back(static_cast<size_t>(best));
        shapes.push_back(items[best].src->points);
        base_height = std::max(base_height, items[best].height);
        line_right =
            std::max(line_right, items[best].rect.x + items[best].rect.width);
      }
    }

    if (shapes.size() < 2) {
      out.push_back(*items[i].src);
      continue;
    }

    // 拼接结果不能比最长的单个成员宽太多，否则就是跨栏误合。
    const std::vector<cv::Point> merged = UnionBoxes(shapes);
    double max_member_long = 0.0;
    std::set<int> branches;
    double score = 0.0;
    for (const auto& s : shapes) {
      max_member_long = std::max(max_member_long, BoxLongSide(s));
    }
    for (size_t member_index : line_members) {
      const MergedBox& mb = *items[member_index].src;
      branches.insert(mb.branches.begin(), mb.branches.end());
      score = std::max(score, mb.score);
    }
    if (BoxLongSide(merged) > max_member_long * m.line_max_width_ratio) {
      // 放弃拼接，原样输出成员。
      for (size_t member_index : line_members) {
        out.push_back(*items[member_index].src);
      }
      continue;
    }

    MergedBox line;
    line.points = merged;
    line.member_boxes = shapes;
    line.branches.assign(branches.begin(), branches.end());
    line.score = score;
    line.member_count = static_cast<int>(shapes.size());
    line.stitched = true;
    line.ref_long_side = BoxLongSide(line.points);
    line.ref_short_side = BoxShortSide(line.points);
    line.text_height = MemberTextHeight(line.member_boxes, line.ref_short_side);
    out.push_back(std::move(line));
  }

  OCR_LOG("StitchSameLine: %zu regions -> %zu regions", boxes.size(),
          out.size());
  return out;
}

void PadRegions(std::vector<MergedBox>* regions, double pad_ratio,
                int image_width, int image_height) {
  if (!regions || pad_ratio <= 0.0 || image_width <= 0 || image_height <= 0) {
    return;
  }
  for (auto& mb : *regions) {
    if (mb.points.size() < 3) continue;
    cv::RotatedRect r = cv::minAreaRect(PointsToMat(mb.points));
    const double short_side = std::min(r.size.width, r.size.height);
    const float pad = static_cast<float>(short_side * pad_ratio);
    r.size.width = std::max(1.0f, r.size.width + 2.0f * pad);
    r.size.height = std::max(1.0f, r.size.height + 2.0f * pad);
    cv::Point2f verts[4];
    r.points(verts);
    std::vector<cv::Point> pts;
    pts.reserve(4);
    for (const auto& v : verts) {
      const int x = std::max(0, std::min(image_width - 1,
                                         static_cast<int>(std::round(v.x))));
      const int y = std::max(0, std::min(image_height - 1,
                                         static_cast<int>(std::round(v.y))));
      pts.emplace_back(x, y);
    }
    mb.points = std::move(pts);
  }
}

std::vector<cv::Point> PadBoxAlongLongSide(const std::vector<cv::Point>& box,
                                           double pad_long_ratio,
                                           int image_width, int image_height) {
  if (box.size() < 3 || pad_long_ratio <= 0.0 || image_width <= 0 ||
      image_height <= 0) {
    return box;
  }
  cv::RotatedRect r = cv::minAreaRect(PointsToMat(box));
  const double short_side = std::min(r.size.width, r.size.height);
  // 行向 pad = pad_long_ratio × 字高；只在长边方向延伸。
  const float pad = static_cast<float>(short_side * pad_long_ratio);
  // minAreaRect 的 size 无方向语义，直接给长边加 2*pad，短边保持。
  if (r.size.width >= r.size.height) {
    r.size.width += 2.0f * pad;
  } else {
    r.size.height += 2.0f * pad;
  }
  cv::Point2f verts[4];
  r.points(verts);
  std::vector<cv::Point> pts;
  pts.reserve(4);
  for (const auto& v : verts) {
    const int x = std::max(
        0, std::min(image_width - 1, static_cast<int>(std::round(v.x))));
    const int y = std::max(
        0, std::min(image_height - 1, static_cast<int>(std::round(v.y))));
    pts.emplace_back(x, y);
  }
  return pts;
}

void PadRegionsAlongLongSide(std::vector<MergedBox>* regions,
                             double pad_long_ratio, int image_width,
                             int image_height) {
  if (!regions || pad_long_ratio <= 0.0 || image_width <= 0 ||
      image_height <= 0) {
    return;
  }
  for (auto& mb : *regions) {
    if (mb.points.size() < 3) continue;
    mb.points = PadBoxAlongLongSide(mb.points, pad_long_ratio, image_width,
                                    image_height);
  }
}

std::vector<cv::Point> PadBox(const std::vector<cv::Point>& box,
                              double pad_ratio, int image_width,
                              int image_height) {
  if (box.size() < 3 || pad_ratio <= 0.0 || image_width <= 0 ||
      image_height <= 0) {
    return box;
  }
  cv::RotatedRect r = cv::minAreaRect(PointsToMat(box));
  const double short_side = std::min(r.size.width, r.size.height);
  const float pad = static_cast<float>(short_side * pad_ratio);
  r.size.width = std::max(1.0f, r.size.width + 2.0f * pad);
  r.size.height = std::max(1.0f, r.size.height + 2.0f * pad);
  cv::Point2f verts[4];
  r.points(verts);
  std::vector<cv::Point> pts;
  pts.reserve(4);
  for (const auto& v : verts) {
    const int x = std::max(
        0, std::min(image_width - 1, static_cast<int>(std::round(v.x))));
    const int y = std::max(
        0, std::min(image_height - 1, static_cast<int>(std::round(v.y))));
    pts.emplace_back(x, y);
  }
  return pts;
}

std::vector<cv::Point> BuildRecognitionWindow(
    const MergedBox& region, double pad_ratio, double pad_long_ratio,
    int image_width, int image_height, double anchor_min_region_ratio) {
  if (region.points.size() < 3) return region.points;

  // -----------------------------------------------------------------------
  // 识别窗 = 「成员统计的水平化重建」。
  //
  // union 会被贴边碎片撑歪：残框把高度撑大一倍后，CRNN 输入里字带只占
  // 一半，笔画细节在缩放到 48px 时丢失、开始读错字（实测「舍→合」、
  // 「篮→籃」）。单选某个成员框也不可靠——贴边残框可能与字带完全咬合、
  // 长边甚至更长。
  //
  // 稳健做法是从成员框群体里**统计**出字带：
  //   字高 h   = 成员短边的中位数（残框是少数，中位数免疫）；
  //   行带中心 = 成员 y 中心的中位数；
  //   行带 x   = 成员与区域 bounding 的 x 并集（保证整行都在）；
  // 窗口 = [x0 − 行向pad, x1 + 行向pad] × [cy − h/2 − 纵向pad, cy + ...]
  // 行向 pad 把「检测框紧邻但检测不到」的字（孤立单横「一」等）交给
  // CRNN —— 识别器窗口里多一段背景不影响读出紧邻的字（实测 pad ≥ 1.0
  // 倍字高即可找回行首「一」）。
  // -----------------------------------------------------------------------
  std::vector<const std::vector<cv::Point>*> members;
  for (const auto& m : region.member_boxes) {
    if (m.size() >= 3) members.push_back(&m);
  }

  if (members.size() >= 2) {
    std::vector<double> shorts;
    std::vector<double> cys;
    double x0 = std::numeric_limits<double>::max();
    double x1 = std::numeric_limits<double>::lowest();
    for (const auto* m : members) {
      const cv::RotatedRect r = cv::minAreaRect(PointsToMat(*m));
      shorts.push_back(std::min(r.size.width, r.size.height));
      cys.push_back(r.center.y);
      const cv::Rect b = BoxBoundingRect(*m);
      x0 = std::min(x0, static_cast<double>(b.x));
      x1 = std::max(x1, static_cast<double>(b.x + b.width));
    }
    // 区域 bounding 的 x 范围一并纳入（含行向外扩与拼接成果）。
    const cv::Rect ub = BoxBoundingRect(region.points);
    x0 = std::min(x0, static_cast<double>(ub.x));
    x1 = std::max(x1, static_cast<double>(ub.x + ub.width));

    const double h = MedianOf(std::move(shorts), BoxShortSide(region.points));
    const double cy = MedianOf(std::move(cys), BoxCenter(region.points).y);
    const double pad_v = h * pad_ratio;
    const double pad_l = h * pad_long_ratio;

    auto clamp_x = [&](double v) {
      return std::max(0, std::min(image_width - 1,
                                  static_cast<int>(std::lround(v))));
    };
    auto clamp_y = [&](double v) {
      return std::max(0, std::min(image_height - 1,
                                  static_cast<int>(std::lround(v))));
    };
    const int wx0 = clamp_x(x0 - pad_l);
    const int wx1 = clamp_x(x1 + pad_l);
    const int wy0 = clamp_y(cy - h / 2.0 - pad_v);
    const int wy1 = clamp_y(cy + h / 2.0 + pad_v);
    if (wx1 - wx0 >= 8 && wy1 - wy0 >= 8) {
      return {cv::Point(wx0, wy0), cv::Point(wx1, wy0), cv::Point(wx1, wy1),
              cv::Point(wx0, wy1)};
    }
  }

  // 成员不足（单框区域等）：退回 union + 双向外扩。
  std::vector<cv::Point> window =
      PadBox(region.points, pad_ratio, image_width, image_height);
  return PadBoxAlongLongSide(window, pad_long_ratio, image_width,
                             image_height);
}

void SortBoxesForReadingOrder(std::vector<MergedBox>* boxes, int sort_order) {
  if (!boxes || sort_order == 2) return;
  if (sort_order == 1) {
    std::stable_sort(boxes->begin(), boxes->end(),
                     [](const MergedBox& a, const MergedBox& b) {
                       return BoxArea(a.points) > BoxArea(b.points);
                     });
    return;
  }
  // 阅读顺序：先按行（y 中心）分桶，桶内按 x 升序。
  // 行容差取区域内最大字高的一半，避免同一行因为轻微倾斜被拆成两行。
  double max_h = 1.0;
  for (const auto& b : *boxes) {
    max_h = std::max(max_h, BoxShortSide(b.points));
  }
  const double tol = std::max(6.0, max_h * 0.5);

  std::vector<std::pair<double, size_t>> keys;
  keys.reserve(boxes->size());
  for (size_t i = 0; i < boxes->size(); ++i) {
    keys.emplace_back(BoxCenter((*boxes)[i].points).y, i);
  }
  std::stable_sort(keys.begin(), keys.end(),
                   [](const std::pair<double, size_t>& a,
                      const std::pair<double, size_t>& b) {
                     return a.first < b.first;
                   });
  // 按 y 分桶
  std::vector<std::vector<size_t>> rows;
  for (const auto& k : keys) {
    if (!rows.empty()) {
      const size_t ref = rows.back().front();
      if (k.first - BoxCenter((*boxes)[ref].points).y <= tol) {
        rows.back().push_back(k.second);
        continue;
      }
    }
    rows.push_back({k.second});
  }
  std::vector<MergedBox> sorted;
  sorted.reserve(boxes->size());
  for (auto& row : rows) {
    std::stable_sort(row.begin(), row.end(),
                     [&](size_t a, size_t b) {
                       return BoxBoundingRect((*boxes)[a].points).x <
                              BoxBoundingRect((*boxes)[b].points).x;
                     });
    for (size_t idx : row) sorted.push_back(std::move((*boxes)[idx]));
  }
  boxes->swap(sorted);
}

// ===========================================================================
// 多变体择优
// ===========================================================================

namespace {

// 解一个 UTF-8 首序列，返回码点并前移下标；遇到非法字节跳过一个。
uint32_t NextCodepoint(const std::string& s, size_t& i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  int len;
  uint32_t cp;
  if (c < 0x80) { len = 1; cp = c; }
  else if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1Fu; }
  else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0Fu; }
  else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07u; }
  else { ++i; return 0xFFFD; }
  if (i + len > s.size()) { ++i; return 0xFFFD; }
  for (int k = 1; k < len; ++k) {
    const unsigned char cb = static_cast<unsigned char>(s[i + k]);
    if ((cb & 0xC0) != 0x80) { ++i; return 0xFFFD; }
    cp = (cp << 6) | (cb & 0x3Fu);
  }
  i += len;
  return cp;
}

// 单个码点的排版宽度（以字高为单位）。
double CodepointEmWidth(uint32_t cp) {
  if (cp == ' ') return 0.35;
  if (cp >= 0x20 && cp <= 0x7E) {
    // ASCII：标点窄，字母数宽。
    const bool alnum = (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
                       (cp >= 'a' && cp <= 'z');
    return alnum ? 0.60 : 0.35;
  }
  // 全角标点 / CJK 统一汉字 / 日文 / 韩文 / CJK 扩展 A
  if ((cp >= 0x3000 && cp <= 0x30FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
      (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) ||
      (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFF00 && cp <= 0xFF60) ||
      (cp >= 0x20000 && cp <= 0x3134F)) {
    return 1.0;
  }
  // 重音拉丁字母、希腊/西里尔等：比 ASCII 宽一点。
  if (cp >= 0xA0 && cp <= 0x206F) return 0.7;
  return 0.7;
}

}  // namespace

double EstimateTextWidthInEm(const std::string& utf8_text) {
  double em = 0.0;
  size_t i = 0;
  while (i < utf8_text.size()) {
    const uint32_t cp = NextCodepoint(utf8_text, i);
    if (cp == 0xFFFD) continue;
    em += CodepointEmWidth(cp);
  }
  return em;
}

double VariantSelectionScore(int32_t mode, const std::string& text,
                             float confidence, double long_side,
                             double short_side, bool raw_crop) {
  const size_t len = [&text] {
    size_t n = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) ++n;
    }
    return n;
  }();

  auto geometry = [&]() -> double {
    const double expected_em = long_side / std::max(1.0, short_side);
    if (expected_em <= 0.5) {
      // 区域几乎是个点（竖排单字等），退回长度打分。
      return static_cast<double>(len) * std::max(confidence, 0.05f);
    }
    const double actual_em = EstimateTextWidthInEm(text);
    const double fill = actual_em / expected_em;
    // fill 偏离 1.0 越远越差；置信度与长度只当平手时的次级排序。
    const double penalty = std::min(std::abs(fill - 1.0), 5.0);
    return (5.0 - penalty) + 0.05 * confidence +
           0.001 * static_cast<double>(len);
  };

  switch (mode) {
    case 0:
      return confidence;
    case 1:
      return static_cast<double>(len) * std::max(confidence, 0.05f);
    case 2:
      return static_cast<double>(len);
    case 4: {
      // 原像素优先 + 几何契合：实测表明二次二值化会偶发性地多吐/少吐
      // 几个字，所以同一区域下“没动过像素”的候选天然更可信；
      // 但只有当它什么都没识别出来时，增强候选才有机会顶上来。
      const double kRawCropBonus = 2.0;
      return geometry() + (raw_crop ? kRawCropBonus : 0.0);
    }
    case 3:
    default:
      return geometry();
  }
}

}  // namespace paddle_ocr
