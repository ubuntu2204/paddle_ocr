// FFI 管线基准：复刻 Dart 侧 runOcrPipeline 的编排（截图 App 走的链路），
// 用 C 原子 API 驱动，能量化「预处理 → 检测 → 合并 → 择优识别」整条链的
// 耗时与结果签名，与 tool/diag/bench.cpp（C++ 内部管线）互为对照。
//
// 用法：
//   tool/diag_build.sh bench_ffi [模型目录] [图片...]
// 环境变量：PP_OCR_BENCH_ROUNDS（默认 2）、PP_OCR_TIMING=1（引擎分阶段计时）
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

#include "ocr_engine.h"
#include "paddle_ocr_timing.h"
#include "pp_ocr_ffi.h"

namespace po = paddle_ocr;

static double NowMs() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
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

// 与 Dart scoreVariant / 原生 VariantSelectionScore(mode=4) 同款公式。
static double ScoreVariant(const std::string& text, double conf, double long_side,
                           double short_side, bool raw_crop, double scale) {
  size_t len = 0;
  for (unsigned char c : text) {
    if ((c & 0xC0) != 0x80) ++len;
  }
  const double expected_em = long_side / (short_side < 1.0 ? 1.0 : short_side);
  double geometry;
  if (expected_em <= 0.5) {
    geometry = static_cast<double>(len) * (conf < 0.05 ? 0.05 : conf);
  } else {
    double actual = 0.0;
    for (size_t i = 0; i < text.size();) {
      unsigned char c = static_cast<unsigned char>(text[i]);
      int n = 1;
      uint32_t cp = c;
      if (c >= 0xF0) { n = 4; cp = c & 0x07; }
      else if (c >= 0xE0) { n = 3; cp = c & 0x0F; }
      else if (c >= 0xC0) { n = 2; cp = c & 0x1F; }
      for (int k = 1; k < n && i + k < text.size(); ++k) {
        cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
      }
      i += static_cast<size_t>(n);
      if (cp == 0x20) actual += 0.35;
      else if (cp >= 0x20 && cp <= 0x7E) {
        const bool alnum = (cp >= 0x30 && cp <= 0x39) ||
                           (cp >= 0x41 && cp <= 0x5A) ||
                           (cp >= 0x61 && cp <= 0x7A);
        actual += alnum ? 0.60 : 0.35;
      } else actual += 1.0;  // 简化：非 ASCII 一律按 CJK 权重
    }
    const double fill = actual / expected_em;
    const double deviation = fill > 1.0 ? fill - 1.0 : 1.0 - fill;
    const double penalty = deviation > 5.0 ? 5.0 : deviation;
    geometry = (5.0 - penalty) + 0.05 * conf + 0.001 * static_cast<double>(len);
  }
  return geometry + (raw_crop ? 2.0 : 0.0) + 0.001 * scale;
}

struct Variant {
  std::string name;
  double scale = 1.0;
  bool rec_source = true;
  std::vector<uint8_t> png;  // 预处理产物（PNG 字节，与 Dart 侧一致）
};

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
  const char* rounds_env = std::getenv("PP_OCR_BENCH_ROUNDS");
  const int rounds = rounds_env ? std::atoi(rounds_env) : 2;

  PpOcrEngine* engine = pp_ocr_create();
  if (!engine) {
    fprintf(stderr, "pp_ocr_create failed\n");
    return 1;
  }
  if (!pp_ocr_initialize(engine, (model_dir + "/det.onnx").c_str(),
                         (model_dir + "/inference.onnx").c_str(),
                         (model_dir + "/ppocr_v6_dict.txt").c_str())) {
    fprintf(stderr, "init failed: %s\n",
            pp_ocr_get_last_error(engine) ? pp_ocr_get_last_error(engine) : "?");
    return 1;
  }

  PpOcrOptions opts;
  pp_ocr_options_preset(PP_OCR_PRESET_SURVEILLANCE, &opts);

  printf("FFI pipeline bench (surveillance) rounds=%d threads=%u\n", rounds,
         std::thread::hardware_concurrency());

  double grand = 0.0;
  for (const auto& path : images) {
    // 读原图字节（PNG/JPEG 文件直接喂 FFI，与 Dart 侧一致）
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
      fprintf(stderr, "load failed: %s\n", path.c_str());
      continue;
    }
    const std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> src(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(src.data()), size);
    f.close();

    printf("\n== %s (%zu bytes)\n", path.c_str(), src.size());
    std::string first_sig;
    double best = 1e18, sum = 0.0;
    double t_pre = 0.0, t_det = 0.0, t_merge = 0.0, t_rec = 0.0;
    for (int r = 0; r < rounds; ++r) {
      t_pre = t_det = t_merge = t_rec = 0.0;
      const double t0 = NowMs();

      // ---- 1. 变体（与 surveillance 分支一一对应）----
      std::vector<Variant> variants;
      auto add_variant = [&](const char* name, double scale, bool rec_source,
                             void (*fill)(PpOcrTransformParams*)) {
        Variant v;
        v.name = name;
        v.scale = scale;
        v.rec_source = rec_source;
        PpOcrTransformParams tp;
        pp_ocr_transform_params_init(&tp);
        fill(&tp);
        PpOcrImageBuffer out{};
        const double ts = NowMs();
        const bool ok = pp_ocr_preprocess_bytes(
            engine, src.data(), static_cast<int>(src.size()), &tp, &out);
        t_pre += NowMs() - ts;
        if (ok && out.data && out.size > 0) {
          v.png.assign(out.data, out.data + out.size);
        }
        variants.push_back(std::move(v));
      };
      add_variant("raw", 1.0, true, [](PpOcrTransformParams*) {});
      add_variant(
          "adaptive_bin", 1.0, false, [](PpOcrTransformParams* tp) {
            tp->binarize_mode = PP_OCR_BINARIZE_ADAPTIVE_GAUSS_INV;
            tp->color_mode = PP_OCR_COLOR_GRAY3;
          });
      add_variant("clahe", 1.0, true, [](PpOcrTransformParams* tp) {
        tp->enhance_mode = PP_OCR_ENHANCE_CLAHE;
        tp->clahe_clip_limit = 3.0;
      });
      add_variant("upscale2x", 2.0, true,
                  [](PpOcrTransformParams* tp) { tp->scale_percent = 200; });
      add_variant("upscale3x", 3.0, true,
                  [](PpOcrTransformParams* tp) { tp->scale_percent = 300; });
      add_variant("denoise_clahe", 1.0, true, [](PpOcrTransformParams* tp) {
        tp->enhance_mode = PP_OCR_ENHANCE_DENOISE_CLAHE;
      });
      add_variant(
          "white_text", 1.0, false, [](PpOcrTransformParams* tp) {
            tp->enhance_mode = PP_OCR_ENHANCE_WHITE_TEXT;
            tp->binarize_mode = PP_OCR_BINARIZE_ADAPTIVE_GAUSS_INV;
            tp->adaptive_block_size = 21;
            tp->adaptive_c = 8.0;
            tp->color_mode = PP_OCR_COLOR_GRAY3;
          });

      // ---- 2. 每路检测（框换算回底图）----
      struct Box {
        double box[8];
        int branch;
        double score;
      };
      std::vector<Box> all_boxes;
      int base_w = 0, base_h = 0;
      for (size_t vi = 0; vi < variants.size(); ++vi) {
        const Variant& v = variants[vi];
        if (v.png.empty()) continue;
        PpOcrOptions vo = opts;
        if (v.scale != 1.0) {
          vo.det_max_side_len =
              static_cast<int32_t>(opts.det_max_side_len * v.scale);
        }
        PpOcrBoxArray detected{};
        const double ts = NowMs();
        const bool ok = pp_ocr_detect_bytes(engine, v.png.data(),
                                            static_cast<int>(v.png.size()), &vo,
                                            &detected);
        t_det += NowMs() - ts;
        if (!ok) {
          continue;
        }
        if (v.scale == 1.0) {
          base_w = detected.image_width;
          base_h = detected.image_height;
        }
        for (int i = 0; i < detected.count; ++i) {
          Box b{};
          for (int j = 0; j < 8; ++j) {
            b.box[j] =
                detected.items[i].box[j] / (v.scale == 1.0 ? 1.0 : v.scale);
          }
          b.branch = static_cast<int>(vi);
          b.score = detected.items[i].score;
          all_boxes.push_back(b);
        }
      }
      if (all_boxes.empty() || base_w <= 0) {
        printf("   round%d: no boxes\n", r);
        continue;
      }

      // ---- 3. 合并为识别窗 ----
      std::vector<PpOcrBoxInput> in(all_boxes.size());
      for (size_t i = 0; i < all_boxes.size(); ++i) {
        for (int j = 0; j < 8; ++j) in[i].box[j] = all_boxes[i].box[j];
        in[i].branch_index = all_boxes[i].branch;
        in[i].score = all_boxes[i].score;
      }
      PpOcrBoxArray merged{};
      const double tm = NowMs();
      const bool merge_ok = pp_ocr_merge_boxes(
          engine, in.data(), static_cast<int>(in.size()), &opts, base_w, base_h,
          &merged);
      t_merge += NowMs() - tm;
      if (!merge_ok || merged.count == 0) {
        printf("   round%d: merge failed\n", r);
        continue;
      }

      // ---- 4. 择优识别 ----
      // PP_OCR_BENCH_BATCH=1 时走批量识别原子 API（一次调用识别全部窗，
      // 原生侧并行）；否则逐个窗调用（Dart 管线旧行为）。两者结果必须一致。
      const bool batch = [] {
        const char* v = std::getenv("PP_OCR_BENCH_BATCH");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
      }();
      std::string sig;
      size_t nrec = 0;
      // 先按变体收集全部窗的识别结果：results[variant_idx][window_idx]
      std::vector<std::vector<std::pair<std::string, double>>> per_variant(
          variants.size());
      for (size_t vi = 0; vi < variants.size(); ++vi) {
        const Variant& v = variants[vi];
        if (!v.rec_source || v.png.empty()) continue;
        per_variant[vi].assign(static_cast<size_t>(merged.count), {"", 0.0});
        if (!batch) {
          for (int m = 0; m < merged.count; ++m) {
            const PpOcrBoxInput& w = merged.items[m];
            double box8[8];
            for (int j = 0; j < 8; ++j) box8[j] = w.box[j] * v.scale;
            PpOcrTextResult tr{};
            const double ts = NowMs();
            const bool ok = pp_ocr_recognize_box_bytes(
                engine, v.png.data(), static_cast<int>(v.png.size()), box8,
                &opts, &tr);
            t_rec += NowMs() - ts;
            if (ok && tr.text != nullptr) {
              ++nrec;
              per_variant[vi][static_cast<size_t>(m)] = {tr.text,
                                                         tr.confidence};
            }
          }
        } else {
          // 批量：一次调用传全部窗（坐标已按该变体缩放）
          std::vector<double> boxes(static_cast<size_t>(merged.count) * 8);
          for (int m = 0; m < merged.count; ++m) {
            const PpOcrBoxInput& w = merged.items[m];
            for (int j = 0; j < 8; ++j) {
              boxes[static_cast<size_t>(m) * 8 + j] = w.box[j] * v.scale;
            }
          }
          PpOcrTextResultArray tra{};
          const double ts = NowMs();
          const bool ok = pp_ocr_recognize_boxes_bytes(
              engine, v.png.data(), static_cast<int>(v.png.size()), boxes.data(),
              merged.count, &opts, &tra);
          t_rec += NowMs() - ts;
          if (!ok || tra.items == nullptr) continue;
          for (int m = 0; m < tra.count && m < merged.count; ++m) {
            ++nrec;
            per_variant[vi][static_cast<size_t>(m)] = {
                tra.items[m].text ? tra.items[m].text : "",
                tra.items[m].confidence};
          }
        }
      }
      for (int m = 0; m < merged.count; ++m) {
        const PpOcrBoxInput& w = merged.items[m];
        double xs0 = 1e18, ys0 = 1e18, xs1 = -1e18, ys1 = -1e18;
        for (int j = 0; j < 4; ++j) {
          xs0 = std::min(xs0, w.box[j * 2]);
          ys0 = std::min(ys0, w.box[j * 2 + 1]);
          xs1 = std::max(xs1, w.box[j * 2]);
          ys1 = std::max(ys1, w.box[j * 2 + 1]);
        }
        const double long_side = std::max(xs1 - xs0, ys1 - ys0);
        const double short_side = std::min(xs1 - xs0, ys1 - ys0);

        std::string best_text;
        double best_conf = 0.0, best_score = -1e18;
        for (size_t vi = 0; vi < variants.size(); ++vi) {
          const Variant& v = variants[vi];
          if (!v.rec_source || v.png.empty()) continue;
          const auto& got = per_variant[vi][static_cast<size_t>(m)];
          if (got.first.empty()) continue;
          const double score =
              ScoreVariant(got.first, got.second, long_side * v.scale,
                           short_side * v.scale, v.scale == 1.0, v.scale);
          if (score > best_score) {
            best_score = score;
            best_text = got.first;
            best_conf = got.second;
          }
        }
        if (best_text.empty()) continue;
        sig += best_text;
        sig += '|';
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", best_conf);
        sig += buf;
        sig += ';';
      }
      const double dt = NowMs() - t0;
      best = std::min(best, dt);
      sum += dt;
      if (r == 0) {
        first_sig = sig;
        printf("   round0: %7.1f ms  windows=%d recognitions=%zu sig=%s\n", dt,
               merged.count, nrec, Fnv1a(sig).c_str());
        printf("     phases: preprocess=%.0f detect=%.0f merge=%.0f "
               "recognize=%.0f (ms)\n",
               t_pre, t_det, t_merge, t_rec);
      } else {
        printf("   round%d: %7.1f ms  %s\n", r, dt,
               sig == first_sig ? "sig-match" : "SIG-MISMATCH!");
      }
    }
    printf("   -> best=%7.1f ms  avg=%7.1f ms\n", best, sum / rounds);
    grand += best;
  }
  printf("\nTOTAL best-sum = %.1f ms\n", grand);
  pp_ocr_destroy(engine);
  return 0;
}
