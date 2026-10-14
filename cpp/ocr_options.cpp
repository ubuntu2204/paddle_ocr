#include "ocr_options.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace paddle_ocr {

namespace {

// 把 mode 值夹到合法枚举范围，越界时回退到 fallback。
template <typename EnumType>
EnumType ClampEnum(int raw, EnumType fallback, int lo, int hi) {
  if (raw < lo || raw > hi) return fallback;
  return static_cast<EnumType>(raw);
}

// 自适应邻域大小的修正：0 是「按目标尺寸自动取值」的哨兵值，属于合法输入，
// 不能当成非法值处理（否则默认选项每跑一次 sanitize 都会被算作「修正过」）。
int NormalizeOrAutoBlockSize(int v, int* fixed) {
  if (v == 0) return 0;
  if (v < 0) {
    ++*fixed;
    return 31;
  }
  if (v % 2 == 0) {
    ++*fixed;
    return v + 1;
  }
  return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// 默认值：这是全仓库唯一的「未显式配置时该用什么」的定义处。
// ---------------------------------------------------------------------------

void FillDefaultPreprocess(PreprocessParams* p) {
  if (!p) return;
  *p = PreprocessParams{};
  p->struct_size = static_cast<int32_t>(sizeof(PreprocessParams));
  p->scale_percent = 100;
  p->binarize_mode = kBinarizeNone;
  p->adaptive_block_size = 31;
  p->adaptive_c = 10.0;
  p->enhance_mode = kEnhanceNone;
  p->clahe_clip_limit = 2.0;
  p->clahe_grid_size = 8;
  p->sharpen_amount = 1.0;
  p->morph_close_kernel = 0;
  p->morph_open_kernel = 0;
  p->color_mode = kColorKeep;
}

void FillDefaultDetect(DetectParams* p) {
  if (!p) return;
  *p = DetectParams{};
  p->struct_size = static_cast<int32_t>(sizeof(DetectParams));
  p->thresh = 0.30;
  p->box_thresh = 0.50;
  p->unclip_ratio = 1.5;
  p->max_side_len = 960;
  p->min_box_size = 3;
  p->min_box_area = 0;
}

void FillDefaultRecognize(RecognizeParams* p) {
  if (!p) return;
  *p = RecognizeParams{};
  p->struct_size = static_cast<int32_t>(sizeof(RecognizeParams));
  p->img_height = 48;
  p->max_img_width = 2048;
  p->enhance_crops = 0;
  p->try_all_branches = 1;
  FillDefaultPreprocess(&p->crop_preprocess);
  // 文本条只有几十像素高，邻域必须比整图小得多；开运算会抹掉
  // 「一」这种单横笔画，因此默认关掉，只保留自适应二值化。
  // adaptive_block_size = 0 表示按文本条实际高度自动取值。
  p->crop_preprocess.binarize_mode = kBinarizeAdaptiveGaussInv;
  p->crop_preprocess.adaptive_block_size = 0;
  p->crop_preprocess.adaptive_c = 5.0;
  p->crop_preprocess.morph_open_kernel = 0;
  p->min_confidence = 0.0;
  p->variant_select = 4;
}

void FillDefaultMerge(MergeParams* p) {
  if (!p) return;
  *p = MergeParams{};
  p->struct_size = static_cast<int32_t>(sizeof(MergeParams));
  p->branches = kBranchMono;
  p->merge_iou_threshold = 0.30;
  p->merge_contain_threshold = 0.75;
  p->stitch_same_line = 0;
  p->line_min_y_overlap = 0.55;
  p->line_max_gap_ratio = 0.60;
  p->line_max_height_diff_ratio = 0.45;
  p->line_max_width_ratio = 4.0;
  p->region_pad_ratio = 0.0;
  p->max_regions = 0;
  p->sort_order = 0;
}

void FillDefaultOptions(OcrOptions* o) {
  if (!o) return;
  *o = OcrOptions{};
  o->struct_size = static_cast<int32_t>(sizeof(OcrOptions));
  FillDefaultDetect(&o->det);
  FillDefaultRecognize(&o->rec);
  FillDefaultMerge(&o->merge);
  o->roi_x = 0;
  o->roi_y = 0;
  o->roi_w = 0;
  o->roi_h = 0;
  o->verbose_stats = 0;
}

// ---------------------------------------------------------------------------
// 预设
// ---------------------------------------------------------------------------

OcrOptions MakePresetOptions(int32_t preset) {
  OcrOptions o;
  FillDefaultOptions(&o);

  switch (preset) {
    case kPresetDocument:
      // 文档/截图：文字与背景分离干净，单分支 + 二值化择优即可。
      o.merge.branches = kBranchRaw | kBranchOtsuBin;
      o.rec.enhance_crops = 2;
      o.rec.crop_preprocess.binarize_mode = kBinarizeOtsuInv;
      o.rec.crop_preprocess.morph_open_kernel = 0;
      break;

    case kPresetLowContrast:
      // 低对比：靠 CLAHE + 自适应二值化把笔画从背景里拉出来。
      o.merge.branches =
          kBranchRaw | kBranchAdaptiveBin | kBranchClahe | kBranchSensitive;
      o.merge.stitch_same_line = 1;
      o.merge.region_pad_ratio = 0.20;
      o.rec.enhance_crops = 2;
      o.det.thresh = 0.26;
      o.det.box_thresh = 0.45;
      break;

    case kPresetSurveillance:
      // 监控画面：叠加字幕（白字 + 半透明底）与压景物的白字同时存在，
      // 且整帧分辨率高、字相对小 —— 需要多分支召回 + 碎框拼接。
      o.merge.branches = kBranchSurveillance;
      o.merge.stitch_same_line = 1;
      o.merge.line_max_gap_ratio = 0.80;
      o.merge.line_max_width_ratio = 6.0;
      // 外扩 0.3 倍字高：把贴边的单横字（「一」）纳回裁剪区。
      o.merge.region_pad_ratio = 0.30;
      // 增强与不增强都跑，由「字符数 × 置信度」择优；
      // 否则自适应二值化会把时间戳里的短横吃掉。
      o.rec.enhance_crops = 2;
      o.rec.try_all_branches = 1;
      // 这里只适度放宽（不是放到最宽）：二值化类分支已经把召回担了，
      // 再降到 0.22 只会多出一堆单字碎框。
      o.det.thresh = 0.26;
      o.det.box_thresh = 0.45;
      o.det.unclip_ratio = 1.7;
      o.det.max_side_len = 1280;
      break;

    case kPresetSmallText:
      // 小字：先放大再检测，同时放宽输入最长边限制。
      o.merge.branches = kBranchRaw | kBranchUpscale2x | kBranchAdaptiveBin;
      o.det.max_side_len = 1600;
      o.rec.enhance_crops = 2;
      o.rec.img_height = 64;
      break;

    case kPresetMaxRecall:
      // 全开：所有分支 + 所有后处理。耗时约为标准模式的 6~10 倍。
      o.merge.branches = kBranchAll;
      o.merge.stitch_same_line = 1;
      o.merge.merge_iou_threshold = 0.20;
      o.merge.merge_contain_threshold = 0.65;
      o.merge.line_max_gap_ratio = 0.80;
      o.merge.region_pad_ratio = 0.30;
      o.rec.enhance_crops = 2;
      o.rec.try_all_branches = 1;
      o.det.thresh = 0.22;
      o.det.box_thresh = 0.40;
      o.det.unclip_ratio = 1.8;
      o.det.max_side_len = 1280;
      break;

    case kPresetStandard:
    default:
      // 保持历史行为：单分支、无二次预处理。
      break;
  }
  return o;
}

// ---------------------------------------------------------------------------
// 合法性修正
// ---------------------------------------------------------------------------

int32_t SanitizeOptions(OcrOptions* o) {
  if (!o) return 0;
  int fixed = 0;

  auto fix_double = [&](double& v, double lo, double hi, double dflt) {
    if (!(v >= lo && v <= hi)) {  // 同时拦截 NaN
      v = dflt;
      fixed++;
    }
  };
  auto fix_int = [&](int32_t& v, int32_t lo, int32_t hi, int32_t dflt) {
    if (v < lo || v > hi) {
      v = dflt;
      fixed++;
    }
  };

  // 检测参数
  fix_double(o->det.thresh, 0.02, 0.95, 0.30);
  fix_double(o->det.box_thresh, 0.02, 0.95, 0.50);
  fix_double(o->det.unclip_ratio, 0.2, 6.0, 1.5);
  fix_int(o->det.max_side_len, 32, 8192, 960);
  fix_int(o->det.min_box_size, 1, 100, 3);
  fix_int(o->det.min_box_area, 0, 100000000, 0);

  // 识别参数
  fix_int(o->rec.img_height, 16, 256, 48);
  fix_int(o->rec.max_img_width, 32, 8192, 2048);
  if (o->rec.enhance_crops < 0) {
    o->rec.enhance_crops = 0;
    fixed++;
  } else if (o->rec.enhance_crops > 2) {
    o->rec.enhance_crops = 2;
    fixed++;
  }
  fix_int(o->rec.try_all_branches, 0, 1, 1);
  fix_double(o->rec.min_confidence, 0.0, 0.99, 0.0);
  fix_int(o->rec.variant_select, 0, 4, 4);

  // 文本条预处理参数
  PreprocessParams& cp = o->rec.crop_preprocess;
  cp.struct_size = static_cast<int32_t>(sizeof(PreprocessParams));
  cp.binarize_mode = ClampEnum(cp.binarize_mode, kBinarizeNone,
                               kBinarizeNone, kBinarizeAuto);
  cp.enhance_mode = ClampEnum(cp.enhance_mode, kEnhanceNone, kEnhanceNone,
                              kEnhanceWhiteText);
  cp.color_mode = ClampEnum(cp.color_mode, kColorKeep, kColorKeep, kColorGray3);
  cp.adaptive_block_size =
      NormalizeOrAutoBlockSize(cp.adaptive_block_size, &fixed);
  fix_double(cp.adaptive_c, -50.0, 50.0, 10.0);
  fix_double(cp.clahe_clip_limit, 0.1, 20.0, 2.0);
  fix_int(cp.clahe_grid_size, 1, 64, 8);
  fix_double(cp.sharpen_amount, 0.0, 10.0, 1.0);
  fix_int(cp.morph_close_kernel, 0, 31, 0);
  fix_int(cp.morph_open_kernel, 0, 31, 0);
  fix_int(cp.scale_percent, 25, 800, 100);

  // 合并参数
  if (o->merge.branches == 0) {
    o->merge.branches = kBranchRaw;
    fixed++;
  }
  // 未知分支位一律清掉，防止宿主传进来一个超大数导致行为未定义。
  o->merge.branches &= kBranchAll;
  if (o->merge.branches == 0) {
    o->merge.branches = kBranchRaw;
    fixed++;
  }
  fix_double(o->merge.merge_iou_threshold, 0.01, 0.99, 0.30);
  fix_double(o->merge.merge_contain_threshold, 0.05, 0.99, 0.75);
  fix_int(o->merge.stitch_same_line, 0, 1, 0);
  fix_double(o->merge.line_min_y_overlap, 0.1, 1.0, 0.55);
  fix_double(o->merge.line_max_gap_ratio, 0.05, 10.0, 0.60);
  fix_double(o->merge.line_max_height_diff_ratio, 0.05, 5.0, 0.45);
  fix_double(o->merge.line_max_width_ratio, 1.0, 50.0, 4.0);
  fix_double(o->merge.region_pad_ratio, 0.0, 2.0, 0.0);
  fix_int(o->merge.max_regions, 0, 10000, 0);
  fix_int(o->merge.sort_order, 0, 2, 0);

  // ROI：负值无意义，归零；宽高为 0 即「整图」。
  if (o->roi_x < 0) { o->roi_x = 0; fixed++; }
  if (o->roi_y < 0) { o->roi_y = 0; fixed++; }
  if (o->roi_w < 0) { o->roi_w = 0; fixed++; }
  if (o->roi_h < 0) { o->roi_h = 0; fixed++; }

  fix_int(o->verbose_stats, 0, 1, 0);
  o->struct_size = static_cast<int32_t>(sizeof(OcrOptions));
  return fixed;
}

// ---------------------------------------------------------------------------
// 可读名称（日志用）
// ---------------------------------------------------------------------------

const char* PresetName(int32_t preset) {
  switch (preset) {
    case kPresetStandard: return "standard";
    case kPresetDocument: return "document";
    case kPresetLowContrast: return "low_contrast";
    case kPresetSurveillance: return "surveillance";
    case kPresetSmallText: return "small_text";
    case kPresetMaxRecall: return "max_recall";
    default: return "unknown";
  }
}

std::string BranchFlagsName(uint32_t branches) {
  static const struct {
    uint32_t flag;
    const char* name;
  } kNames[] = {
      {kBranchRaw, "raw"},
      {kBranchAdaptiveBin, "adaptive_bin"},
      {kBranchOtsuBin, "otsu_bin"},
      {kBranchClahe, "clahe"},
      {kBranchUpscale2x, "upscale2x"},
      {kBranchSensitive, "sensitive"},
      {kBranchSharpen, "sharpen"},
      {kBranchDenoiseClahe, "denoise_clahe"},
      {kBranchWhiteText, "white_text"},
  };
  std::ostringstream oss;
  bool first = true;
  for (const auto& e : kNames) {
    if (branches & e.flag) {
      if (!first) oss << '|';
      oss << e.name;
      first = false;
    }
  }
  if (first) oss << "(none)";
  return oss.str();
}

OcrOptions OptionsFromMapLookup(const MapLookup& lookup) {
  OcrOptions o;
  FillDefaultOptions(&o);
  if (!lookup.get_double || !lookup.get_int) return o;

  const auto& g = lookup;
  o.det.thresh = g.get_double("detThresh", o.det.thresh);
  o.det.box_thresh = g.get_double("detBoxThresh", o.det.box_thresh);
  o.det.unclip_ratio = g.get_double("detUnclipRatio", o.det.unclip_ratio);
  o.det.max_side_len = g.get_int("detMaxSideLen", o.det.max_side_len);
  o.det.min_box_size = g.get_int("detMinBoxSize", o.det.min_box_size);
  o.det.min_box_area = g.get_int("detMinBoxArea", o.det.min_box_area);

  o.rec.img_height = g.get_int("recImgHeight", o.rec.img_height);
  o.rec.max_img_width = g.get_int("recMaxImgWidth", o.rec.max_img_width);
  o.rec.enhance_crops = g.get_int("recEnhanceCrops", o.rec.enhance_crops);
  o.rec.try_all_branches = g.get_int("recTryAllBranches", o.rec.try_all_branches);
  o.rec.min_confidence = g.get_double("recMinConfidence", o.rec.min_confidence);
  o.rec.variant_select = g.get_int("recVariantSelect", o.rec.variant_select);

  PreprocessParams& cp = o.rec.crop_preprocess;
  cp.binarize_mode =
      static_cast<BinarizeMode>(g.get_int("cropBinarizeMode", cp.binarize_mode));
  cp.adaptive_block_size =
      g.get_int("cropAdaptiveBlockSize", cp.adaptive_block_size);
  cp.adaptive_c = g.get_double("cropAdaptiveC", cp.adaptive_c);
  cp.enhance_mode =
      static_cast<EnhanceMode>(g.get_int("cropEnhanceMode", cp.enhance_mode));
  cp.clahe_clip_limit =
      g.get_double("cropClaheClipLimit", cp.clahe_clip_limit);
  cp.clahe_grid_size = g.get_int("cropClaheGridSize", cp.clahe_grid_size);
  cp.sharpen_amount = g.get_double("cropSharpenAmount", cp.sharpen_amount);
  cp.morph_close_kernel =
      g.get_int("cropMorphCloseKernel", cp.morph_close_kernel);
  cp.morph_open_kernel =
      g.get_int("cropMorphOpenKernel", cp.morph_open_kernel);
  cp.color_mode = static_cast<ColorMode>(g.get_int("cropColorMode",
                                                   cp.color_mode));

  o.merge.branches =
      static_cast<uint32_t>(g.get_int("branches", static_cast<int32_t>(
                                                       o.merge.branches)));
  o.merge.merge_iou_threshold =
      g.get_double("mergeIouThreshold", o.merge.merge_iou_threshold);
  o.merge.merge_contain_threshold =
      g.get_double("mergeContainThreshold", o.merge.merge_contain_threshold);
  o.merge.stitch_same_line =
      g.get_int("stitchSameLine", o.merge.stitch_same_line);
  o.merge.line_min_y_overlap =
      g.get_double("lineMinYOverlap", o.merge.line_min_y_overlap);
  o.merge.line_max_gap_ratio =
      g.get_double("lineMaxGapRatio", o.merge.line_max_gap_ratio);
  o.merge.line_max_height_diff_ratio =
      g.get_double("lineMaxHeightDiffRatio", o.merge.line_max_height_diff_ratio);
  o.merge.line_max_width_ratio =
      g.get_double("lineMaxWidthRatio", o.merge.line_max_width_ratio);
  o.merge.region_pad_ratio =
      g.get_double("regionPadRatio", o.merge.region_pad_ratio);
  o.merge.max_regions = g.get_int("maxRegions", o.merge.max_regions);
  o.merge.sort_order = g.get_int("sortOrder", o.merge.sort_order);

  o.roi_x = g.get_int("roiX", o.roi_x);
  o.roi_y = g.get_int("roiY", o.roi_y);
  o.roi_w = g.get_int("roiW", o.roi_w);
  o.roi_h = g.get_int("roiH", o.roi_h);
  o.verbose_stats = g.get_int("verboseStats", o.verbose_stats);

  SanitizeOptions(&o);
  return o;
}

}  // namespace paddle_ocr
