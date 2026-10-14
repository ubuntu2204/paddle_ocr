// 临时诊断程序：验证多分支预处理 + 同行拼接能否把整行字识别全。
// 不属于插件产物，仅用于调参与回归确认。
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "ocr_engine.h"

namespace po = paddle_ocr;

static void Dump(const char* tag, const std::vector<po::OcrBoxResult>& results) {
  printf("\n===== %s : %zu results =====\n", tag, results.size());
  for (size_t i = 0; i < results.size(); ++i) {
    const auto& r = results[i];
    int ymin = std::min({r.box[0].y, r.box[1].y, r.box[2].y, r.box[3].y});
    int xmin = std::min({r.box[0].x, r.box[1].x, r.box[2].x, r.box[3].x});
    int xmax = std::max({r.box[0].x, r.box[1].x, r.box[2].x, r.box[3].x});
    printf("[%2zu] y=%3d x=%4d..%-4d conf=%.3f  %s\n", i, ymin, xmin, xmax,
           r.confidence, r.text.c_str());
  }
  fflush(stdout);
}

static void Run(po::OcrEngine& engine, const cv::Mat& img, const char* tag,
                po::OcrOptions opts) {
  opts.verbose_stats = 1;
  po::SanitizeOptions(&opts);
  printf("\n--- %s: branches=%s stitch=%d enhance_crops=%d det=(%.2f,%.2f,%.1f,%d)\n",
         tag, po::BranchFlagsName(opts.merge.branches).c_str(),
         opts.merge.stitch_same_line, opts.rec.enhance_crops, opts.det.thresh,
         opts.det.box_thresh, opts.det.unclip_ratio, opts.det.max_side_len);
  Dump(tag, engine.Recognize(img, opts));
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
    fprintf(stderr, "init failed: %s\n", engine.GetLastError().c_str());
    return 1;
  }

  cv::Mat src = cv::imread(img_path, cv::IMREAD_COLOR);
  if (src.empty()) {
    fprintf(stderr, "load failed: %s\n", img_path.c_str());
    return 1;
  }
  printf("image %dx%d\n", src.cols, src.rows);

  // 1) 基线（等价于历史行为）
  Run(engine, src, "standard", po::MakePresetOptions(po::kPresetStandard));

  // 2) 单路 white_text：验证「白字提取 + 自适应二值化」本身能不能召回整行
  {
    po::OcrOptions o = po::MakePresetOptions(po::kPresetStandard);
    o.merge.branches = po::kBranchWhiteText;
    o.merge.stitch_same_line = 0;
    Run(engine, src, "white_text_only", o);
  }

  // 3) white_text + sensitive
  {
    po::OcrOptions o = po::MakePresetOptions(po::kPresetStandard);
    o.merge.branches = po::kBranchWhiteText | po::kBranchSensitive;
    o.merge.stitch_same_line = 0;
    Run(engine, src, "white_text+sensitive", o);
  }

  // 4) 监控预设（多分支 + 拼接 + 文本条增强）
  Run(engine, src, "surveillance", po::MakePresetOptions(po::kPresetSurveillance));

  // 5) 全开
  Run(engine, src, "max_recall", po::MakePresetOptions(po::kPresetMaxRecall));

  return 0;
}
