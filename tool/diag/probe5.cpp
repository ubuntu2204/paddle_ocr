// 验证四个原子操作 C API（宿主编排管线的插件接口）。
// 复现 Dart OcrPipeline 的流程：预处理变体 → 检测 → 合并 → 择优识别。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "pp_ocr_ffi.h"

static std::vector<uint8_t> ReadFile(const std::string& path) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return {};
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data(size);
  if (fread(data.data(), 1, size, f) != static_cast<size_t>(size)) data.clear();
  fclose(f);
  return data;
}

int main(int argc, char** argv) {
  const std::string model_dir = argc > 1 ? argv[1] : "model";
  const std::string img_path =
      argc > 2 ? argv[2]
               : (std::getenv("HOME") +
                  std::string("/图片/无法识别/一舍蓝球场东北路口球G66-5724.png"));

  PpOcrEngine* engine = pp_ocr_create();
  if (!pp_ocr_initialize(engine, (model_dir + "/det.onnx").c_str(),
                         (model_dir + "/inference.onnx").c_str(),
                         (model_dir + "/ppocr_v6_dict.txt").c_str())) {
    fprintf(stderr, "init failed: %s\n", pp_ocr_get_last_error(engine));
    return 1;
  }

  std::vector<uint8_t> base = ReadFile(img_path);
  if (base.empty()) {
    fprintf(stderr, "read image failed\n");
    return 1;
  }

  PpOcrOptions opts;
  pp_ocr_options_preset(PP_OCR_PRESET_SURVEILLANCE, &opts);

  // 变体 1：底图（raw）
  std::vector<std::vector<uint8_t>> variant_bytes;
  std::vector<double> variant_scales;
  std::vector<const uint8_t*> variant_data;
  std::vector<int> variant_size;
  variant_bytes.push_back(std::move(base));
  variant_scales.push_back(1.0);

  // 变体 2：3x 放大（原子 1：预处理）
  {
    PpOcrTransformParams tp;
    pp_ocr_transform_params_init(&tp);
    tp.scale_percent = 300;
    PpOcrImageBuffer out{};
    if (pp_ocr_preprocess_bytes(engine, variant_bytes[0].data(),
                                static_cast<int>(variant_bytes[0].size()),
                                &tp, &out)) {
      variant_bytes.emplace_back(out.data, out.data + out.size);
      variant_scales.push_back(3.0);
      printf("preprocess: 3x variant %d bytes\n", out.size);
    } else {
      fprintf(stderr, "preprocess failed: %s\n", pp_ocr_get_last_error(engine));
      return 1;
    }
  }
  for (size_t i = 0; i < variant_bytes.size(); ++i) {
    variant_data.push_back(variant_bytes[i].data());
    variant_size.push_back(static_cast<int>(variant_bytes[i].size()));
  }

  // 各变体检测（原子 2），框换算回底图坐标。
  struct Detected {
    double box[8];
    int branch;
    double score;
  };
  std::vector<Detected> all;
  int base_w = 0, base_h = 0;
  for (size_t i = 0; i < variant_data.size(); ++i) {
    PpOcrBoxArray boxes{};
    if (!pp_ocr_detect_bytes(engine, variant_data[i], variant_size[i], &opts,
                             &boxes)) {
      fprintf(stderr, "detect[%zu] failed: %s\n", i,
              pp_ocr_get_last_error(engine));
      return 1;
    }
    printf("detect[%zu]: %d boxes on %dx%d\n", i, boxes.count,
           boxes.image_width, boxes.image_height);
    if (i == 0) {
      base_w = boxes.image_width;
      base_h = boxes.image_height;
    }
    for (int b = 0; b < boxes.count; ++b) {
      Detected d{};
      for (int j = 0; j < 8; ++j) {
        d.box[j] = boxes.items[b].box[j] / variant_scales[i];
      }
      d.branch = boxes.items[b].branch_index;
      d.score = boxes.items[b].score;
      all.push_back(d);
    }
  }

  // 合并（原子 3）。
  std::vector<PpOcrBoxInput> inputs(all.size());
  for (size_t i = 0; i < all.size(); ++i) {
    std::memcpy(inputs[i].box, all[i].box, sizeof(double) * 8);
    inputs[i].branch_index = all[i].branch;
    inputs[i].score = all[i].score;
  }
  PpOcrBoxArray windows{};
  if (!pp_ocr_merge_boxes(engine, inputs.data(), static_cast<int>(inputs.size()),
                          &opts, base_w, base_h, &windows)) {
    fprintf(stderr, "merge failed: %s\n", pp_ocr_get_last_error(engine));
    return 1;
  }
  printf("merge: %d windows on %dx%d\n", windows.count, base_w, base_h);

  // 择优识别（原子 4）。
  for (int w = 0; w < windows.count; ++w) {
    const double* box = windows.items[w].box;
    std::string best_text;
    double best_conf = 0.0;
    for (size_t i = 0; i < variant_data.size(); ++i) {
      double scaled[8];
      for (int j = 0; j < 8; ++j) scaled[j] = box[j] * variant_scales[i];
      PpOcrTextResult out{};
      if (!pp_ocr_recognize_box_bytes(engine, variant_data[i], variant_size[i],
                                      scaled, &opts, &out)) {
        continue;
      }
      if (out.text && strlen(out.text) > best_text.size()) {
        best_text = out.text;
        best_conf = out.confidence;
      }
    }
    if (best_text.empty()) continue;
    printf("window[%d]: '%s' (%.3f)\n", w, best_text.c_str(), best_conf);
  }

  pp_ocr_destroy(engine);
  printf("ATOMS OK\n");
  return 0;
}
