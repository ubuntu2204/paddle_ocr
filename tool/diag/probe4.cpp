// 排查实验（终）：识别窗质量假说验证。
// union 框被贴顶碎框撑高后，窗口内字带只占一半，CRNN 缩放到 48px 后
// 笔画细节丢失。对比「union 框」vs「精准检测框」在同等工作下的识别。
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "ocr_engine.h"
#include "ocr_preprocess.h"

namespace po = paddle_ocr;

static std::vector<cv::Point> RectToBox(const cv::Rect& r) {
  return {cv::Point(r.x, r.y),
          cv::Point(r.x + r.width - 1, r.y),
          cv::Point(r.x + r.width - 1, r.y + r.height - 1),
          cv::Point(r.x, r.y + r.height - 1)};
}

int main(int argc, char** argv) {
  const std::string model_dir = argc > 1 ? argv[1] : "model";
  const std::string img_path =
      argc > 2 ? argv[2]
               : (std::getenv("HOME") +
                  std::string("/图片/无法识别/一舍蓝球场东北路口球G66-5724.png"));

  po::OcrEngine engine;
  if (!engine.Initialize(model_dir + "/det.onnx", model_dir + "/inference.onnx",
                         model_dir + "/ppocr_v6_dict.txt")) {
    fprintf(stderr, "init failed\n");
    return 1;
  }

  cv::Mat src = cv::imread(img_path, cv::IMREAD_COLOR);
  if (src.empty()) return 1;

  po::DetectParams det;
  po::FillDefaultDetect(&det);
  det.thresh = 0.26;
  det.box_thresh = 0.45;
  det.unclip_ratio = 1.7;
  det.max_side_len = 2560;

  po::RecognizeParams rec;
  po::FillDefaultRecognize(&rec);
  rec.enhance_crops = 0;

  // upscale 2x 变体（surveillance 的识别主力分支）
  cv::Mat img2x = engine.ApplyImageTransform(src, [] {
    po::PreprocessParams p;
    po::FillDefaultPreprocess(&p);
    p.scale_percent = 200;
    return p;
  }());

  printf("detect on 2x...\n");
  auto boxes = engine.DetectOnImage(img2x, det);
  // 找最长的框（字幕行「舍篮球场东北路口球G66-5724」）
  const po::DetectedBox* line = nullptr;
  double best_long = 0;
  for (const auto& b : boxes) {
    cv::Rect r = cv::boundingRect(b.points);
    if (r.y < 100 && r.width > best_long) {
      best_long = r.width;
      line = &b;
    }
  }
  if (!line) {
    printf("no line box\n");
    return 1;
  }
  cv::Rect det_rect = cv::boundingRect(line->points);
  printf("detected line (2x): x=%d..%d y=%d..%d (h=%d)\n", det_rect.x,
         det_rect.x + det_rect.width, det_rect.y, det_rect.y + det_rect.height,
         det_rect.height);

  // 模拟 union 框：把行框垂直方向撑大一倍（贴顶碎框的效果），
  // 水平方向左移覆盖背景（行向外扩的效果）。
  const int h2 = det_rect.height * 2;
  cv::Rect union_like(det_rect.x - det_rect.height, det_rect.y - det_rect.height / 2,
                      det_rect.width + det_rect.height, h2);
  union_like &= cv::Rect(0, 0, img2x.cols, img2x.rows);

  struct Case {
    const char* name;
    cv::Rect rect;
  };
  std::vector<Case> cases = {
      {"det_box_raw", det_rect},
      {"det_box_pad1.2h",
       cv::Rect(det_rect.x - static_cast<int>(det_rect.height * 1.2), det_rect.y,
                det_rect.width + static_cast<int>(det_rect.height * 2.4),
                det_rect.height) &
           cv::Rect(0, 0, img2x.cols, img2x.rows)},
      {"union_like_raw", union_like},
      {"union_like_pad1.2h",
       cv::Rect(union_like.x - static_cast<int>(union_like.height * 0.6),
                union_like.y,
                union_like.width + static_cast<int>(union_like.height * 1.2),
                union_like.height) &
           cv::Rect(0, 0, img2x.cols, img2x.rows)},
      // union 框但高度收紧回字高（中心不变）
      {"union_tightened",
       cv::Rect(union_like.x, det_rect.y, union_like.width, det_rect.height)},
  };

  for (const auto& c : cases) {
    if (c.rect.width < 8 || c.rect.height < 8) continue;
    auto t1 = engine.RecognizeBox(img2x, RectToBox(c.rect), rec);
    cv::Rect r1 = c.rect & cv::Rect(0, 0, src.cols, src.rows);
    auto t2 = engine.RecognizeBox(src, RectToBox(r1), rec);
    printf("  %-20s 2x:'%s'  raw:'%s'\n", c.name, t1.first.c_str(),
           t2.first.c_str());
    fflush(stdout);
  }
  return 0;
}
