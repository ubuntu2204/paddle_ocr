// 排查实验：右上角「一舍篮球场东北路口球G66-5724」为什么丢「一舍」。
// 对 ROI 条带跑一组变换 × 检测矩阵，每个变体把中间图存到 /tmp/ocr_probe/
// 便于肉眼核对二值化质量。不属于插件产物。
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/stat.h>

#include <opencv2/opencv.hpp>

#include "ocr_engine.h"
#include "ocr_preprocess.h"

namespace po = paddle_ocr;

static void EnsureDir(const char* dir) {
  mkdir(dir, 0755);
}

static void Save(const std::string& name, const cv::Mat& img) {
  const std::string path = std::string("/tmp/ocr_probe/") + name + ".png";
  cv::imwrite(path, img);
  printf("  saved %s\n", path.c_str());
}

// V-S 白字响应（与 ApplyWhiteTextExtract 一致，但返回单通道，便于再加工）。
static cv::Mat WhiteResponse(const cv::Mat& bgr) {
  cv::Mat hsv;
  cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
  std::vector<cv::Mat> ch;
  cv::split(hsv, ch);
  cv::Mat resp;
  cv::subtract(ch[2], ch[1], resp);
  return resp;
}

// 每个变体独立检测 + 识别（实验专用：不合并、不择优，看原始召回）。
static std::vector<po::OcrBoxResult> DetectAndRecognize(
    po::TextDetector& detector, po::TextRecognizer& recognizer,
    const cv::Mat& variant_in, const po::DetectParams& det,
    const po::RecognizeParams& rec, int branch) {
  // 检测器固定吃 3 通道。
  cv::Mat variant = variant_in;
  if (variant.channels() == 1) {
    cv::cvtColor(variant, variant, cv::COLOR_GRAY2BGR);
  }
  std::vector<po::OcrBoxResult> out;
  for (const auto& db : detector.DetectScored(variant, det, branch)) {
    auto t = recognizer.RecognizeRegion(variant, db.points, rec);
    po::OcrBoxResult r;
    r.box = db.points;
    r.text = t.first;
    r.confidence = t.second;
    out.push_back(std::move(r));
  }
  return out;
}

static void Dump(const char* tag, const std::vector<po::OcrBoxResult>& rs) {
  printf("  ---- %s: %zu boxes\n", tag, rs.size());
  for (const auto& r : rs) {
    int xmin = std::min({r.box[0].x, r.box[1].x, r.box[2].x, r.box[3].x});
    int xmax = std::max({r.box[0].x, r.box[1].x, r.box[2].x, r.box[3].x});
    int ymin = std::min({r.box[0].y, r.box[1].y, r.box[2].y, r.box[3].y});
    printf("      y=%d x=%d..%-4d conf=%.3f '%s'\n", ymin, xmin, xmax,
           r.confidence, r.text.c_str());
  }
  fflush(stdout);
}

int main(int argc, char** argv) {
  EnsureDir("/tmp/ocr_probe");
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
  printf("image %dx%d\n", src.cols, src.rows);

  // 右上角条带：从海报左缘一直到图像右缘。
  cv::Rect roi(720, 0, src.cols - 720, 60);
  cv::Mat band = src(roi).clone();
  Save("00_band_raw", band);

  po::RecognizeParams rec;
  po::FillDefaultRecognize(&rec);
  rec.enhance_crops = 0;

  po::DetectParams det;   // sensitive 档
  po::FillDefaultDetect(&det);
  det.thresh = 0.26;
  det.box_thresh = 0.45;
  det.unclip_ratio = 1.7;
  det.max_side_len = 1280;

  // 变体工厂：name -> (Mat, 是否打印检测)
  struct Variant {
    const char* name;
    cv::Mat img;
  };
  std::vector<Variant> variants;

  {
    // 1) raw 2x / 4x
    cv::Mat b2, b4;
    cv::resize(band, b2, cv::Size(band.cols * 2, band.rows * 2), 0, 0,
               cv::INTER_CUBIC);
    cv::resize(band, b4, cv::Size(band.cols * 4, band.rows * 4), 0, 0,
               cv::INTER_CUBIC);
    variants.push_back({"01_band_raw2x", b2});
    variants.push_back({"02_band_raw4x", b4});

    // 2) V-S 响应（灰）2x
    cv::Mat resp = WhiteResponse(band);
    Save("03_band_white_resp", resp);
    cv::Mat r2;
    cv::resize(resp, r2, cv::Size(resp.cols * 2, resp.rows * 2), 0, 0,
               cv::INTER_CUBIC);
    variants.push_back({"04_band_white2x", r2});

    // 3) V-S + 自适应二值化（当前 white_text 分支的整图做法）2x
    cv::Mat bin;
    cv::adaptiveThreshold(r2, bin, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY_INV, 41, 8.0);
    Save("05_band_white_bin2x", bin);
    cv::Mat bin3;
    cv::cvtColor(bin, bin3, cv::COLOR_GRAY2BGR);
    variants.push_back({"06_band_white_bin2x3ch", bin3});

    // 4) 灰度 top-hat：提取「比局部背景亮的细结构」——单横「一」正属此类。
    //    字高约 22px（band 内），2x 后 44px，核取 31 覆盖笔画邻域。
    cv::Mat gray, g2, tophat;
    cv::cvtColor(band, gray, cv::COLOR_BGR2GRAY);
    cv::resize(gray, g2, cv::Size(gray.cols * 2, gray.rows * 2), 0, 0,
               cv::INTER_CUBIC);
    cv::morphologyEx(g2, tophat, cv::MORPH_TOPHAT,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(31, 31)));
    cv::normalize(tophat, tophat, 0, 255, cv::NORM_MINMAX);
    Save("07_band_tophat2x", tophat);
    variants.push_back({"08_band_tophat2x3ch", tophat});

    // 5) V-S 的 top-hat（先压彩色再提细结构）
    cv::Mat resp2, wtop;
    cv::resize(resp, resp2, cv::Size(resp.cols * 2, resp.rows * 2), 0, 0,
               cv::INTER_CUBIC);
    cv::morphologyEx(resp2, wtop, cv::MORPH_TOPHAT,
                     cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(31, 31)));
    cv::normalize(wtop, wtop, 0, 255, cv::NORM_MINMAX);
    Save("09_band_whitetophat2x", wtop);
    variants.push_back({"10_band_whitetophat2x3ch", wtop});

    // 6) 黄绿色字幕色域提取：字幕是黄绿色（高 V、中等 S、H 在黄绿区间）。
    cv::Mat hsv, mask;
    cv::cvtColor(band, hsv, cv::COLOR_BGR2HSV);
    // OpenCV H ∈ [0,180)。黄绿 ≈ 20..45，饱和度 > 60，亮度 > 120。
    cv::inRange(hsv, cv::Scalar(20, 60, 120), cv::Scalar(45, 255, 255), mask);
    Save("11_band_yellow_mask", mask);
    cv::Mat mask2;
    cv::resize(mask, mask2, cv::Size(mask.cols * 2, mask.rows * 2), 0, 0,
               cv::INTER_NEAREST);
    cv::Mat mask3;
    cv::cvtColor(mask2, mask3, cv::COLOR_GRAY2BGR);
    variants.push_back({"12_band_yellow2x3ch", mask3});

    // 7) CLAHE(gray) 2x
    cv::Mat lab = g2.clone();
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(3.0, cv::Size(8, 8));
    clahe->apply(lab, lab);
    Save("13_band_clahe2x", lab);
    variants.push_back({"14_band_clahe2x3ch", lab});
  }

  for (size_t i = 0; i < variants.size(); ++i) {
    const auto& v = variants[i];
    printf("\n=== variant %s (%dx%d)\n", v.name, v.img.cols, v.img.rows);
    Save(v.name, v.img);
    Dump(v.name, DetectAndRecognize(detector, recognizer, v.img, det, rec,
                                    static_cast<int>(i)));
  }

  // 对照组：整图 4x + white 响应（max_side_len 调大），看整图管线在大尺度下
  // 能否直接召回「一舍」。
  printf("\n=== full-image 4x white-response\n");
  {
    po::RecognizeParams rec2 = rec;
    po::DetectParams det4 = det;
    det4.max_side_len = 4096;
    cv::Mat resp = WhiteResponse(src);
    cv::Mat resp4;
    cv::resize(resp, resp4, cv::Size(resp.cols * 4, resp.rows * 4), 0, 0,
               cv::INTER_CUBIC);
    Save("15_full_white4x", resp4);
    cv::Mat resp4b;
    cv::adaptiveThreshold(resp4, resp4b, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY_INV, 81, 8.0);
    Save("16_full_white_bin4x", resp4b);
    cv::Mat resp4b3;
    cv::cvtColor(resp4b, resp4b3, cv::COLOR_GRAY2BGR);
    Dump("full_white_bin4x",
         DetectAndRecognize(detector, recognizer, resp4b3, det4, rec2, 99));
  }
  return 0;
}
