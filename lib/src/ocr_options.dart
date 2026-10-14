import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'pp_ocr_ffi_bindings.dart';

/// 预处理用的二值化模式。
///
/// 值与原生侧 `PP_OCR_BINARIZE_*` 一一对应，不要改动顺序。
enum BinarizeMode {
  /// 不二值化，保持彩色输入。
  none(0),

  /// 全局 Otsu + 反色（文字变黑、背景变白）。适合光照均匀的文档。
  otsuInverted(1),

  /// 全局 Otsu + 正色。适合黑底白字。
  otsuPlain(2),

  /// 高斯加权自适应阈值 + 反色。对光照渐变、压在景物上的白字最鲁棒。
  adaptiveGaussianInverted(3),

  /// 均值自适应阈值 + 反色。比高斯版快，噪点略多。
  adaptiveMeanInverted(4),

  /// 自动挑选 Otsu / 自适应，并自动判断文字极性。
  auto(5);

  const BinarizeMode(this.value);
  final int value;

  static BinarizeMode fromValue(int v) =>
      values.firstWhere((e) => e.value == v, orElse: () => none);
}

/// 对比度 / 噪声增强模式。
enum EnhanceMode {
  none(0),

  /// 限制对比度的自适应直方图均衡（CLAHE）。
  clahe(1),

  /// 非锐化掩模，强化细笔画边缘。
  sharpen(2),

  /// 先 CLAHE 再锐化。
  claheSharpen(3),

  /// 双边滤波去噪，抹掉块噪但保留笔画边缘。
  denoise(4),

  /// 先去噪再 CLAHE。
  denoiseClahe(5),

  /// 白色叠加字幕提取：按「亮度高且饱和度低」重绘图像，把压在树丛 /
  /// 广告牌 / 路面上的监控字幕从背景里剥离出来。
  whiteText(6);

  const EnhanceMode(this.value);
  final int value;

  static EnhanceMode fromValue(int v) =>
      values.firstWhere((e) => e.value == v, orElse: () => none);
}

/// 通道处理。
enum ColorMode {
  /// 保持 BGR 彩色。
  keep(0),

  /// 转灰度后复制成 3 通道。
  gray3(1);

  const ColorMode(this.value);
  final int value;

  static ColorMode fromValue(int v) =>
      values.firstWhere((e) => e.value == v, orElse: () => keep);
}

/// 检测分支。每一路 = 一种预处理 + 一套检测参数；多路并行检测后合并去重，
/// 可以救回任何单一分支漏掉的文字。
enum OcrBranch {
  /// 原始图 + 标准检测参数。任何配置都应至少包含它。
  raw(1 << 0),

  /// 高斯自适应二值化。
  adaptiveBinarize(1 << 1),

  /// 全局 Otsu 二值化。
  otsuBinarize(1 << 2),

  /// CLAHE 对比度增强。
  clahe(1 << 3),

  /// 2 倍放大并同步放宽输入边长，救回极小字。
  upscale2x(1 << 4),

  /// 更宽松的阈值组合（不改图像，只降阈值）。
  sensitive(1 << 5),

  /// 锐化。
  sharpen(1 << 6),

  /// 去噪 + CLAHE，面向高压缩监控画面。
  denoiseClahe(1 << 7),

  /// 白色叠加字幕提取 + 自适应二值化。
  /// 监控 / 直播画面漏检的主因就在这里。
  whiteText(1 << 8),

  /// 3 倍放大：2x 对 20px 级小字仍会偶发繁简误读（篮→籃），3x 下笔画
  /// 细节充足、实测完全正确。比 2x 再慢一档，按需开启。
  upscale3x(1 << 9);

  const OcrBranch(this.bit);

  /// 与原生侧 `PP_OCR_BRANCH_*` 一致的位值。
  final int bit;

  /// 把一组分支压成位掩码。
  static int maskOf(Iterable<OcrBranch> branches) {
    var mask = 0;
    for (final b in branches) {
      mask |= b.bit;
    }
    return mask;
  }

  /// 从位掩码还原分支集合。
  static Set<OcrBranch> branchesOf(int mask) =>
      values.where((b) => mask & b.bit != 0).toSet();

  /// 单路（等价于历史行为）。
  static const Set<OcrBranch> mono = {raw};

  /// 监控画面推荐组合。
  static const Set<OcrBranch> surveillance = {
    raw,
    adaptiveBinarize,
    clahe,
    upscale2x,
    upscale3x,
    sensitive,
    denoiseClahe,
    whiteText,
  };

  /// 全部开启，最慢最全。
  static const Set<OcrBranch> all = {
    raw,
    adaptiveBinarize,
    otsuBinarize,
    clahe,
    upscale2x,
    upscale3x,
    sensitive,
    sharpen,
    denoiseClahe,
    whiteText,
  };
}

/// 原生预设。默认值只在原生侧定义，Dart 不复制一份，避免两边漂移；
/// 用 [OcrOptions.preset] 可以把某个预设完整读成 Dart 对象再微调。
enum OcrPreset {
  /// 单分支，等价于历史默认行为。
  standard(0),

  /// 文档 / 屏幕截图。
  document(1),

  /// 低对比：路牌、压暗背景上的白字。
  lowContrast(2),

  /// 监控画面：叠加字幕 + 复杂景物。白字压绿树这类漏检的推荐值。
  surveillance(3),

  /// 小字 / 远距离：以放大为主。
  smallText(4),

  /// 全分支 + 全部后处理，最慢但最全。
  maxRecall(5);

  const OcrPreset(this.id);
  final int id;
}

/// 多变体择优策略：同一区域在多张图上各识别一次时，用哪个结果胜出。
enum VariantSelection {
  /// 只看置信度。PP-OCR 的置信度极易饱和，一般不足以区分。
  confidence(0),

  /// 字符数 × 置信度。
  lengthTimesConfidence(1),

  /// 只看字符数（召回优先，容易把噪点读成乱码）。
  length(2),

  /// 结果宽度与区域长宽比是否契合。同时惩罚「读少了」和「读油了」。
  geometryFit(3),

  /// 在 [geometryFit] 基础上，额外给未二次增强的原像素候选加分。默认。
  geometryFitRawFirst(4);

  const VariantSelection(this.value);
  final int value;

  static VariantSelection fromValue(int v) =>
      values.firstWhere((e) => e.value == v, orElse: () => geometryFitRawFirst);
}

/// 结果排序方式。
enum ResultSortOrder {
  /// 阅读顺序：先按行（上→下），行内左→右。
  readingOrder(0),

  /// 面积降序。
  areaDescending(1),

  /// 保持管道产出顺序。
  none(2);

  const ResultSortOrder(this.value);
  final int value;

  static ResultSortOrder fromValue(int v) =>
      values.firstWhere((e) => e.value == v, orElse: () => readingOrder);
}

/// OCR 管道的可调选项。
///
/// 三种典型用法：
///
/// 1. 直接用预设（推荐，绝大多数场景只需一行）：
/// ```dart
/// final results = await ocr.recognizeImage(
///   path,
///   options: OcrOptions.preset(OcrPreset.surveillance),
/// );
/// ```
///
/// 2. 在预设上微调：
/// ```dart
/// final opts = OcrOptions.preset(OcrPreset.surveillance)
///     .copyWith(detThresh: 0.22);
/// ```
///
/// 3. 从零构造：`const OcrOptions(branches: {OcrBranch.raw, OcrBranch.whiteText})`。
///
/// [OcrOptions.preset] 会把原生侧的预设取值读回来变成一个普通对象，因此
/// 「预设里有什么值」这件事只在原生侧定义一处。
class OcrOptions {
  /// 用显式值构造。未指定的字段取「标准」默认值（与原生侧一致）。
  const OcrOptions({
    this.detThresh = 0.30,
    this.detBoxThresh = 0.50,
    this.detUnclipRatio = 1.5,
    this.detMaxSideLen = 960,
    this.detMinBoxSize = 3,
    this.detMinBoxArea = 0,
    this.recImgHeight = 48,
    this.recMaxImgWidth = 2048,
    this.recEnhanceCrops = 0,
    this.recTryAllBranches = true,
    this.recMinConfidence = 0.0,
    this.variantSelection = VariantSelection.geometryFitRawFirst,
    this.cropBinarizeMode = BinarizeMode.adaptiveGaussianInverted,
    this.cropAdaptiveBlockSize = 0,
    this.cropAdaptiveC = 5.0,
    this.cropEnhanceMode = EnhanceMode.none,
    this.cropClaheClipLimit = 2.0,
    this.cropClaheGridSize = 8,
    this.cropSharpenAmount = 1.0,
    this.cropMorphCloseKernel = 0,
    this.cropMorphOpenKernel = 0,
    this.cropColorMode = ColorMode.keep,
    this.branches = const {OcrBranch.raw},
    this.mergeIouThreshold = 0.30,
    this.mergeContainThreshold = 0.75,
    this.stitchSameLine = false,
    this.lineMinYOverlap = 0.55,
    this.lineMaxGapRatio = 0.60,
    this.lineMaxHeightDiffRatio = 0.45,
    this.lineMaxWidthRatio = 4.0,
    this.regionPadRatio = 0.0,
    this.regionPadLongRatio = 0.0,
    this.maxRegions = 0,
    this.sortOrder = ResultSortOrder.readingOrder,
    this.roiX = 0,
    this.roiY = 0,
    this.roiW = 0,
    this.roiH = 0,
    this.verboseStats = false,
    this.sourcePreset,
  });

  /// 取某个原生预设并把它的取值实体化。
  ///
  /// 需要原生库已可加载；纯 Dart 单测里请直接用默认构造函数。
  factory OcrOptions.preset(OcrPreset preset) => OcrOptions.fromNative(preset);

  /// 快速构造：只开某几路分支，其余用标准默认值。
  const OcrOptions.branches(Set<OcrBranch> branches) : this(branches: branches);

  /// 监控 / 低对比画面预设（多分支 + 同行拼接 + 择优识别）。
  ///
  /// 就是 [OcrOptions.preset]`(OcrPreset.surveillance)` 的简写，取值全部
  /// 来自原生侧；需要原生库可加载。想手写一份不依赖原生的配置，用
  /// `const OcrOptions(branches: OcrBranch.surveillance, stitchSameLine: true)`。
  factory OcrOptions.surveillance() =>
      OcrOptions.fromNative(OcrPreset.surveillance);

  // ---- 文本检测（DB）----
  /// 概率图二值化阈值：越低越敏感（召回高、噪框多）。
  final double detThresh;

  /// 文本框平均得分阈值：低于该值的框被丢弃。
  final double detBoxThresh;

  /// 文本框外扩比例。
  final double detUnclipRatio;

  /// 送入检测模型的最长边。超过会等比缩小（小字细节会损失）。
  final int detMaxSideLen;

  /// 最小短边（像素），小于该值的框视为噪点。
  final int detMinBoxSize;

  /// 最小框面积（像素²），0 = 不限制。
  final int detMinBoxArea;

  // ---- 文本识别（CRNN + CTC）----
  /// 识别输入固定高度（PP-OCR 为 48）。
  final int recImgHeight;

  /// 识别输入最大宽度，超宽文本横向压缩。
  final int recMaxImgWidth;

  /// 文本条二次预处理：0 = 从不，1 = 总是，2 = 两种都跑并择优。
  final int recEnhanceCrops;

  /// 同一区域是否在所有分支图上都试一次识别。
  final bool recTryAllBranches;

  /// 丢弃低于该置信度的结果。0.0 = 全部保留。
  final double recMinConfidence;

  /// 多变体择优策略。
  final VariantSelection variantSelection;

  // ---- 文本条二次预处理的参数（仅 recEnhanceCrops != 0 时生效）----
  final BinarizeMode cropBinarizeMode;

  /// 自适应阈值邻域大小；0 = 按文本条实际高度自动取值。
  final int cropAdaptiveBlockSize;
  final double cropAdaptiveC;
  final EnhanceMode cropEnhanceMode;
  final double cropClaheClipLimit;
  final int cropClaheGridSize;
  final double cropSharpenAmount;

  /// 开运算核半径（0 = 关闭）。二值化后文字是黑字白底，开运算让黑色
  /// 扩张：既能消除白色小噪点，又能把断裂的笔画连成整字。
  final int cropMorphOpenKernel;

  /// 闭运算核半径（0 = 关闭）。方向与开运算相反，会抹掉黑色小碎点，
  /// 但同时也会吃掉「一」这类细笔画，慎用。
  final int cropMorphCloseKernel;
  final ColorMode cropColorMode;

  // ---- 多分支与区域合并 ----
  /// 启用哪些检测分支。
  final Set<OcrBranch> branches;

  /// 两个框的 IoU 超过该值即视为同一区域。
  final double mergeIouThreshold;

  /// 小框被大框覆盖的比例超过该值即视为重复。
  final double mergeContainThreshold;

  /// 是否把同一行内水平相邻的碎框拼成整行再识别。
  /// 这是把「球场东北路口球」+「G66-57」+「24」还原成完整一行的关键。
  final bool stitchSameLine;

  /// 判定同一行所需的垂直重叠比例（按较短框高度计）。
  final double lineMinYOverlap;

  /// 判定同一行允许的最大水平间距（相对字高的倍数，逐跳计算）。
  final double lineMaxGapRatio;

  /// 判定同一行允许的高度差比例。
  final double lineMaxHeightDiffRatio;

  /// 拼接结果相对最长成员的行宽上限，防止跨栏误合。
  final double lineMaxWidthRatio;

  /// 区域外扩比例（相对字高）。用于把贴边的单横笔画纳回裁剪区。
  final double regionPadRatio;

  /// 识别窗沿行方向向两侧外扩的比例（相对字高）。
  ///
  /// DB 检测对紧邻行的孤立字符（典型：单横「一」）没有响应，把识别窗
  /// 沿行向延伸 1.0~1.5 倍字高后，CRNN 能把紧邻的字读出来。监控字幕
  /// 实测 1.2 即可找回行首「一」。0 = 不延伸。
  final double regionPadLongRatio;

  /// 最终结果数量上限，0 = 不限制。
  final int maxRegions;

  /// 结果排序方式。
  final ResultSortOrder sortOrder;

  // ---- 感兴趣区域 ----
  /// 只识别该矩形内的文字，结果坐标仍以原图为基准。
  /// 四个值全为 0 表示整图。
  final int roiX;
  final int roiY;
  final int roiW;
  final int roiH;

  // ---- 诊断 ----
  /// 把每路分支的检出数量打到原生日志。
  final bool verboseStats;

  /// 仅用于展示 / 调试：这份参数最初来自哪个预设。
  final OcrPreset? sourcePreset;

  /// 是否等价于「不做任何额外预处理」，可用于快速短路。
  bool get isPlain =>
      branches.length == 1 &&
      branches.contains(OcrBranch.raw) &&
      !stitchSameLine &&
      recEnhanceCrops == 0 &&
      roiW == 0 &&
      roiH == 0;

  /// 分支位掩码。
  int get branchMask => OcrBranch.maskOf(branches);

  /// 是否指定了有效 ROI。
  bool get hasRoi => roiW > 0 && roiH > 0;

  /// 从原生侧读出某个预设的完整取值。
  static OcrOptions fromNative(OcrPreset preset) {
    final ptr = calloc<PpOcrOptionsC>();
    try {
      PpOcrFfiBindings.instance.optionsPreset(preset.id, ptr);
      return fromStruct(ptr.ref, sourcePreset: preset);
    } finally {
      calloc.free(ptr);
    }
  }

  /// 从 C 结构体还原成 Dart 对象。
  static OcrOptions fromStruct(PpOcrOptionsC s, {OcrPreset? sourcePreset}) {
    return OcrOptions(
      detThresh: s.detThresh,
      detBoxThresh: s.detBoxThresh,
      detUnclipRatio: s.detUnclipRatio,
      detMaxSideLen: s.detMaxSideLen,
      detMinBoxSize: s.detMinBoxSize,
      detMinBoxArea: s.detMinBoxArea,
      recImgHeight: s.recImgHeight,
      recMaxImgWidth: s.recMaxImgWidth,
      recEnhanceCrops: s.recEnhanceCrops,
      recTryAllBranches: s.recTryAllBranches != 0,
      recMinConfidence: s.recMinConfidence,
      variantSelection: VariantSelection.fromValue(s.recVariantSelect),
      cropBinarizeMode: BinarizeMode.fromValue(s.cropBinarizeMode),
      cropAdaptiveBlockSize: s.cropAdaptiveBlockSize,
      cropAdaptiveC: s.cropAdaptiveC,
      cropEnhanceMode: EnhanceMode.fromValue(s.cropEnhanceMode),
      cropClaheClipLimit: s.cropClaheClipLimit,
      cropClaheGridSize: s.cropClaheGridSize,
      cropSharpenAmount: s.cropSharpenAmount,
      cropMorphCloseKernel: s.cropMorphCloseKernel,
      cropMorphOpenKernel: s.cropMorphOpenKernel,
      cropColorMode: ColorMode.fromValue(s.cropColorMode),
      branches: OcrBranch.branchesOf(s.branches),
      mergeIouThreshold: s.mergeIouThreshold,
      mergeContainThreshold: s.mergeContainThreshold,
      stitchSameLine: s.stitchSameLine != 0,
      lineMinYOverlap: s.lineMinYOverlap,
      lineMaxGapRatio: s.lineMaxGapRatio,
      lineMaxHeightDiffRatio: s.lineMaxHeightDiffRatio,
      lineMaxWidthRatio: s.lineMaxWidthRatio,
      regionPadRatio: s.regionPadRatio,
      regionPadLongRatio: s.regionPadLongRatio,
      maxRegions: s.maxRegions,
      sortOrder: ResultSortOrder.fromValue(s.sortOrder),
      roiX: s.roiX,
      roiY: s.roiY,
      roiW: s.roiW,
      roiH: s.roiH,
      verboseStats: s.verboseStats != 0,
      sourcePreset: sourcePreset,
    );
  }

  /// 覆盖部分字段。
  OcrOptions copyWith({
    double? detThresh,
    double? detBoxThresh,
    double? detUnclipRatio,
    int? detMaxSideLen,
    int? detMinBoxSize,
    int? detMinBoxArea,
    int? recImgHeight,
    int? recMaxImgWidth,
    int? recEnhanceCrops,
    bool? recTryAllBranches,
    double? recMinConfidence,
    VariantSelection? variantSelection,
    BinarizeMode? cropBinarizeMode,
    int? cropAdaptiveBlockSize,
    double? cropAdaptiveC,
    EnhanceMode? cropEnhanceMode,
    double? cropClaheClipLimit,
    int? cropClaheGridSize,
    double? cropSharpenAmount,
    int? cropMorphCloseKernel,
    int? cropMorphOpenKernel,
    ColorMode? cropColorMode,
    Set<OcrBranch>? branches,
    double? mergeIouThreshold,
    double? mergeContainThreshold,
    bool? stitchSameLine,
    double? lineMinYOverlap,
    double? lineMaxGapRatio,
    double? lineMaxHeightDiffRatio,
    double? lineMaxWidthRatio,
    double? regionPadRatio,
    double? regionPadLongRatio,
    int? maxRegions,
    ResultSortOrder? sortOrder,
    int? roiX,
    int? roiY,
    int? roiW,
    int? roiH,
    bool? verboseStats,
    OcrPreset? sourcePreset,
  }) {
    return OcrOptions(
      detThresh: detThresh ?? this.detThresh,
      detBoxThresh: detBoxThresh ?? this.detBoxThresh,
      detUnclipRatio: detUnclipRatio ?? this.detUnclipRatio,
      detMaxSideLen: detMaxSideLen ?? this.detMaxSideLen,
      detMinBoxSize: detMinBoxSize ?? this.detMinBoxSize,
      detMinBoxArea: detMinBoxArea ?? this.detMinBoxArea,
      recImgHeight: recImgHeight ?? this.recImgHeight,
      recMaxImgWidth: recMaxImgWidth ?? this.recMaxImgWidth,
      recEnhanceCrops: recEnhanceCrops ?? this.recEnhanceCrops,
      recTryAllBranches: recTryAllBranches ?? this.recTryAllBranches,
      recMinConfidence: recMinConfidence ?? this.recMinConfidence,
      variantSelection: variantSelection ?? this.variantSelection,
      cropBinarizeMode: cropBinarizeMode ?? this.cropBinarizeMode,
      cropAdaptiveBlockSize:
          cropAdaptiveBlockSize ?? this.cropAdaptiveBlockSize,
      cropAdaptiveC: cropAdaptiveC ?? this.cropAdaptiveC,
      cropEnhanceMode: cropEnhanceMode ?? this.cropEnhanceMode,
      cropClaheClipLimit: cropClaheClipLimit ?? this.cropClaheClipLimit,
      cropClaheGridSize: cropClaheGridSize ?? this.cropClaheGridSize,
      cropSharpenAmount: cropSharpenAmount ?? this.cropSharpenAmount,
      cropMorphCloseKernel: cropMorphCloseKernel ?? this.cropMorphCloseKernel,
      cropMorphOpenKernel: cropMorphOpenKernel ?? this.cropMorphOpenKernel,
      cropColorMode: cropColorMode ?? this.cropColorMode,
      branches: branches ?? this.branches,
      mergeIouThreshold: mergeIouThreshold ?? this.mergeIouThreshold,
      mergeContainThreshold:
          mergeContainThreshold ?? this.mergeContainThreshold,
      stitchSameLine: stitchSameLine ?? this.stitchSameLine,
      lineMinYOverlap: lineMinYOverlap ?? this.lineMinYOverlap,
      lineMaxGapRatio: lineMaxGapRatio ?? this.lineMaxGapRatio,
      lineMaxHeightDiffRatio:
          lineMaxHeightDiffRatio ?? this.lineMaxHeightDiffRatio,
      lineMaxWidthRatio: lineMaxWidthRatio ?? this.lineMaxWidthRatio,
      regionPadRatio: regionPadRatio ?? this.regionPadRatio,
      regionPadLongRatio: regionPadLongRatio ?? this.regionPadLongRatio,
      maxRegions: maxRegions ?? this.maxRegions,
      sortOrder: sortOrder ?? this.sortOrder,
      roiX: roiX ?? this.roiX,
      roiY: roiY ?? this.roiY,
      roiW: roiW ?? this.roiW,
      roiH: roiH ?? this.roiH,
      verboseStats: verboseStats ?? this.verboseStats,
      sourcePreset: sourcePreset ?? this.sourcePreset,
    );
  }

  /// 只限制识别区域的快捷方法。
  OcrOptions withRoi(int x, int y, int w, int h) =>
      copyWith(roiX: x, roiY: y, roiW: w, roiH: h);

  /// 取消 ROI 限制。
  OcrOptions withoutRoi() => copyWith(roiX: 0, roiY: 0, roiW: 0, roiH: 0);

  /// 写入 C 结构体。调用方负责分配 / 释放。
  void writeTo(PpOcrOptionsC s) {
    s.structSize = PpOcrFfiBindings.instance.optionsSize();
    s.detThresh = detThresh;
    s.detBoxThresh = detBoxThresh;
    s.detUnclipRatio = detUnclipRatio;
    s.detMaxSideLen = detMaxSideLen;
    s.detMinBoxSize = detMinBoxSize;
    s.detMinBoxArea = detMinBoxArea;
    s.recImgHeight = recImgHeight;
    s.recMaxImgWidth = recMaxImgWidth;
    s.recEnhanceCrops = recEnhanceCrops;
    s.recTryAllBranches = recTryAllBranches ? 1 : 0;
    s.recMinConfidence = recMinConfidence;
    s.recVariantSelect = variantSelection.value;
    s.cropBinarizeMode = cropBinarizeMode.value;
    s.cropAdaptiveBlockSize = cropAdaptiveBlockSize;
    s.cropAdaptiveC = cropAdaptiveC;
    s.cropEnhanceMode = cropEnhanceMode.value;
    s.cropClaheClipLimit = cropClaheClipLimit;
    s.cropClaheGridSize = cropClaheGridSize;
    s.cropSharpenAmount = cropSharpenAmount;
    s.cropMorphCloseKernel = cropMorphCloseKernel;
    s.cropMorphOpenKernel = cropMorphOpenKernel;
    s.cropColorMode = cropColorMode.value;
    s.branches = branchMask;
    s.mergeIouThreshold = mergeIouThreshold;
    s.mergeContainThreshold = mergeContainThreshold;
    s.stitchSameLine = stitchSameLine ? 1 : 0;
    s.lineMinYOverlap = lineMinYOverlap;
    s.lineMaxGapRatio = lineMaxGapRatio;
    s.lineMaxHeightDiffRatio = lineMaxHeightDiffRatio;
    s.lineMaxWidthRatio = lineMaxWidthRatio;
    s.regionPadRatio = regionPadRatio;
    s.regionPadLongRatio = regionPadLongRatio;
    s.maxRegions = maxRegions;
    s.sortOrder = sortOrder.value;
    s.roiX = roiX;
    s.roiY = roiY;
    s.roiW = roiW;
    s.roiH = roiH;
    s.verboseStats = verboseStats ? 1 : 0;
  }

  /// 分配一个填好本对象取值的 C 结构体指针。调用方需 `calloc.free`。
  Pointer<PpOcrOptionsC> toNativeStruct() {
    final ptr = calloc<PpOcrOptionsC>();
    writeTo(ptr.ref);
    return ptr;
  }

  @override
  String toString() =>
      'OcrOptions(preset: ${sourcePreset?.name ?? 'custom'}, '
      'branches: {${branches.map((b) => b.name).join(', ')}}, '
      'det(thresh: $detThresh, box: $detBoxThresh, unclip: $detUnclipRatio, '
      'maxSide: $detMaxSideLen), '
      'stitch: $stitchSameLine, pad: $regionPadRatio, '
      'padLong: $regionPadLongRatio, '
      'enhanceCrops: $recEnhanceCrops, '
      'roi: ${hasRoi ? '$roiX,$roiY,$roiW,$roiH' : 'full'})';
}
