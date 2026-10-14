// 定点探针：用 ROI 只跑顶部字幕带，在各种预处理变体上分别走完整管道，
// 看哪一路能把「一舍篮球场东北路口球G66-5724」读全。用于调分支参数。
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

  po::OcrEngine engine;
  if (!engine.Initialize(model_dir + "/det.onnx", model_dir + "/inference.onnx",
                         model_dir + "/ppocr_v6_dict.txt")) {
    fprintf(stderr, "init failed\n");
    return 1;
  }
  cv::Mat src = cv::imread(img_path, cv::IMREAD_COLOR);
  if (src.empty()) { fprintf(stderr, "load failed\n"); return 1; }

  po::OcrOptions base = po::MakePresetOptions(po::kPresetSurveillance);
  // 顶部字幕带（含时间戳与位置两行）
  base.roi_x = 700; base.roi_y = 0; base.roi_w = 350; base.roi_h = 60;

  struct Case { const char* name; uint32_t flag; };
  const Case cases[] = {
      {"raw", po::kBranchRaw},
      {"adaptive_bin", po::kBranchAdaptiveBin},
      {"otsu_bin", po::kBranchOtsuBin},
      {"clahe", po::kBranchClahe},
      {"upscale2x", po::kBranchUpscale2x},
      {"sharpen", po::kBranchSharpen},
      {"denoise_clahe", po::kBranchDenoiseClahe},
      {"white_text", po::kBranchWhiteText},
  };

  for (int pad10 = 0; pad10 <= 6; pad10 += 3) {
    printf("\n### region_pad_ratio=%.1f\n", pad10 / 10.0);
    for (const auto& c : cases) {
      for (int enh = 0; enh <= 1; ++enh) {
        po::OcrOptions o = base;
        o.merge.branches = c.flag;
        o.merge.stitch_same_line = 1;
        o.merge.region_pad_ratio = pad10 / 10.0;
        o.rec.enhance_crops = enh;
        o.rec.try_all_branches = 0;
        auto r = engine.Recognize(src, o);
        printf("  %-14s enh=%d -> ", c.name, enh);
        for (const auto& item : r) printf("[%s]", item.text.c_str());
        printf("\n");
      }
    }
  }
  return 0;
}
