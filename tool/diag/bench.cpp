// 识别速度基准工具：对若干张图重复跑指定预设，统计墙钟耗时，并输出
// 结果的稳定签名（文本拼接），供优化前后做「速度↑且结果不变」的对照。
//
// 用法：
//   tool/diag_build.sh bench [模型目录] [图片1 图片2 ...]
//
// 环境变量：
//   PP_OCR_BENCH_PRESET  预设编号：3=surveillance 5=max_recall 0=默认单分支
//   PP_OCR_BENCH_ROUNDS  每张图重复次数（默认 3）
//   PP_OCR_TIMING        设 1 打开引擎分阶段计时（pipeline/det/rec）
//
// 产物不在插件里，仅用于本地调参与回归。
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

#include "ocr_engine.h"
#include "paddle_ocr_timing.h"

namespace po = paddle_ocr;

static double NowMs() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 结果签名：把全部文本按顺序拼接（附置信度分档），用于跨版本比对。
// 只要签名一致就认为识别行为未变。
static std::string Signature(const std::vector<po::OcrBoxResult>& results) {
  std::string sig;
  char buf[64];
  for (const auto& r : results) {
    sig += r.text;
    sig += '|';
    snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(r.confidence));
    sig += buf;
    sig += ';';
  }
  return sig;
}

static std::string Fnv1a(const std::string& s) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char buf[32];
  snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

int main(int argc, char** argv) {
  const std::string model_dir = argc > 1 ? argv[1] : "model";

  std::vector<std::string> images;
  if (argc > 2) {
    for (int i = 2; i < argc; ++i) images.push_back(argv[i]);
  } else {
    const char* home = std::getenv("HOME");
    images.push_back(std::string(home ? home : "") +
                     "/图片/无法识别/一舍篮球场东北路口球G66-5724.png");
  }

  const char* preset_env = std::getenv("PP_OCR_BENCH_PRESET");
  const int preset = preset_env ? std::atoi(preset_env) : 3;  // 默认 surveillance
  const char* rounds_env = std::getenv("PP_OCR_BENCH_ROUNDS");
  const int rounds = rounds_env ? std::atoi(rounds_env) : 3;

  po::OcrEngine engine;
  if (!engine.Initialize(model_dir + "/det.onnx", model_dir + "/inference.onnx",
                         model_dir + "/ppocr_v6_dict.txt")) {
    fprintf(stderr, "init failed: %s\n", engine.GetLastError().c_str());
    return 1;
  }

  printf("preset=%d (%s) rounds=%d threads=%u\n", preset,
         po::PresetName(preset), rounds, std::thread::hardware_concurrency());

  double grand_total = 0.0;
  int grand_regions = 0;
  for (const auto& path : images) {
    cv::Mat src = cv::imread(path, cv::IMREAD_COLOR);
    if (src.empty()) {
      fprintf(stderr, "load failed: %s\n", path.c_str());
      continue;
    }
    printf("\n== %s (%dx%d)\n", path.c_str(), src.cols, src.rows);

    po::OcrOptions opts = po::MakePresetOptions(preset);
    std::string first_sig;
    double best = 1e18, sum = 0.0;
    size_t nregions = 0;
    for (int r = 0; r < rounds; ++r) {
      if (po::TimingEnabled()) po::PaddleOcrTimer::Reset();
      const double t0 = NowMs();
      auto results = engine.Recognize(src, opts);
      const double dt = NowMs() - t0;
      best = std::min(best, dt);
      sum += dt;
      nregions = results.size();
      grand_regions += static_cast<int>(results.size());
      std::string sig = Signature(results);
      if (r == 0) {
        first_sig = sig;
        printf("   round0: %6.1f ms  %zu regions  sig=%s\n", dt, results.size(),
               Fnv1a(sig).c_str());
        for (size_t i = 0; i < results.size(); ++i) {
          printf("     [%2zu] conf=%.3f %s\n", i, results[i].confidence,
                 results[i].text.c_str());
        }
      } else {
        printf("   round%d: %6.1f ms  %s\n", r, dt,
               sig == first_sig ? "sig-match" : "SIG-MISMATCH!");
      }
      if (po::TimingEnabled()) {
        char title[128];
        snprintf(title, sizeof(title), "%s round%d", path.c_str(), r);
        po::PaddleOcrTimer::Report(title);
      }
    }
    printf("   -> best=%6.1f ms  avg=%6.1f ms  regions=%zu\n", best,
           sum / rounds, nregions);
    grand_total += best;
  }
  printf("\nTOTAL best-sum = %.1f ms over %d regions\n", grand_total,
         grand_regions);
  return 0;
}
