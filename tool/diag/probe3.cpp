// 排查实验（续）：检测抓不到「一」，但识别器能不能读出它？
// 对检出行框沿行方向向左/右外扩不同字高数，直接喂识别器看结果。
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "ocr_engine.h"

namespace po = paddle_ocr;

int main(int argc, char** argv) {
  const std::string model_dir = argc > 1 ? argv[1] : "model";
  const std::string img_path =
      argc > 2 ? argv[2]
               : (std::getenv("HOME") +
                  std::string("/图片/无法识别/一舍蓝球场东北路口球G66-5724.png"));

  po::TextDetector detector;
  po::TextRecognizer recognizer;
  if (!detector.Initialize(model_dir + "/det.onnx") ||
      !recognizer.Initialize(model_dir + "/inference.onnx",
                             model_dir + "/ppocr_v6_dict.txt")) {
    fprintf(stderr, "init failed\n");
    return 1;
  }

  cv::Mat src = cv::imread(img_path, cv::IMREAD_COLOR);
  if (src.empty()) return 1;

  // 条带 = 右上角字幕区
  cv::Rect roi(720, 0, src.cols - 720, 60);
  cv::Mat band = src(roi).clone();
  cv::Mat band2, band3, band4;
  cv::resize(band, band2, cv::Size(band.cols * 2, band.rows * 2), 0, 0,
             cv::INTER_CUBIC);
  cv::resize(band, band3, cv::Size(band.cols * 3, band.rows * 3), 0, 0,
             cv::INTER_CUBIC);
  cv::resize(band, band4, cv::Size(band.cols * 4, band.rows * 4), 0, 0,
             cv::INTER_CUBIC);

  po::DetectParams det;
  po::FillDefaultDetect(&det);
  det.thresh = 0.26;
  det.box_thresh = 0.45;
  det.unclip_ratio = 1.7;
  det.max_side_len = 1280;

  po::RecognizeParams rec;
  po::FillDefaultRecognize(&rec);
  rec.enhance_crops = 0;

  for (const auto& [name, img] :
       std::vector<std::pair<const char*, cv::Mat>>{
           {"band2x", band2}, {"band3x", band3}, {"band4x", band4}}) {
    auto boxes = detector.DetectScored(img, det, 0);
    if (boxes.empty()) {
      printf("%s: no boxes\n", name);
      continue;
    }
    // 取最长的框（整行「舍篮球场东北路口球G66-5724」）
    const auto& line = *std::max_element(
        boxes.begin(), boxes.end(), [](const po::DetectedBox& a,
                                        const po::DetectedBox& b) {
          return cv::boundingRect(a.points).width <
                 cv::boundingRect(b.points).width;
        });
    cv::Rect r = cv::boundingRect(line.points);
    const double h = r.height;  // 字高（2x 图上 ≈ 60px）
    printf("\n%s: line box x=%d..%d y=%d..%d (h=%.0f)\n", name, r.x,
           r.x + r.width, r.y, r.y + r.height, h);

    // 沿行方向向左右各外扩 k 倍字高，裁剪后直接识别。
    for (double k : {0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0}) {
      const int pad = static_cast<int>(std::round(h * k));
      cv::Rect win(r.x - pad, r.y, r.width + 2 * pad, r.height);
      win &= cv::Rect(0, 0, img.cols, img.rows);
      if (win.width < 8 || win.height < 8) continue;
      std::vector<cv::Point> box = {
          cv::Point(win.x, win.y),
          cv::Point(win.x + win.width - 1, win.y),
          cv::Point(win.x + win.width - 1, win.y + win.height - 1),
          cv::Point(win.x, win.y + win.height - 1)};
      auto t = recognizer.RecognizeRegion(img, box, rec);
      printf("  pad=%.1fh win=x[%d..%d] -> '%s' (%.3f)\n", k, win.x,
             win.x + win.width, t.first.c_str(), t.second);
      fflush(stdout);
    }
  }
  return 0;
}
