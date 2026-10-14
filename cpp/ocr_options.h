#ifndef OCR_OPTIONS_H_
#define OCR_OPTIONS_H_

#include <cstdint>
#include <functional>
#include <string>

namespace paddle_ocr {

// ===========================================================================
// 本文件定义 OCR 管道的全部可调参数。
//
// 设计约束（重要）：
//   1. 所有枚举都以 int32_t 为底层类型，且取值一经发布**永不重排**——
//      它们会原样穿过 FFI 结构体（见 cpp/pp_ocr_ffi.h）交给宿主 App，
//      已编译的外部调用方依赖这些整数值。
//   2. 所有结构体都是可 memcpy 的 POD（无虚函数、无容器、无 std::string
//      字段），字符串一律用 char 数组表示，以便 FFI 侧直接按内存布局传递。
//   3. 新增字段只能追加到结构体末尾，并通过 `struct_size` 字段做版本
//      协商：调用方先填 sizeof，被调方据此判断新字段是否存在。
// ===========================================================================

// ---------------------------------------------------------------------------
// 二值化模式
//
// 用途：把「白字压在复杂背景上」（监控叠加字幕、路牌、低对比扫描件）这类
// 单分支检测容易碎裂的图，转成高对比的黑字白底，让 DB 检测器重新聚焦。
// ---------------------------------------------------------------------------
enum BinarizeMode : int32_t {
  // 不二值化，保持彩色输入。
  kBinarizeNone = 0,
  // 全局 Otsu 阈值 + 反色（文字→黑，背景→白）。适合光照均匀的文档。
  kBinarizeOtsuInv = 1,
  // 全局 Otsu 阈值 + 正色（文字→白，背景→黑）。适合黑底白字。
  kBinarizeOtsuPlain = 2,
  // 高斯加权自适应阈值 + 反色。对光照渐变、压缩噪点最鲁棒，
  // 是「低对比叠加字幕」的首选。
  kBinarizeAdaptiveGaussInv = 3,
  // 均值自适应阈值 + 反色。比高斯版更快，噪点略多。
  kBinarizeAdaptiveMeanInv = 4,
  // 自动：按图内对比度挑选 Otsu / 自适应，并自动判断是否需要反色。
  kBinarizeAuto = 5,
};

// ---------------------------------------------------------------------------
// 增强模式
// ---------------------------------------------------------------------------
enum EnhanceMode : int32_t {
  kEnhanceNone = 0,
  // 限制对比度的自适应直方图均衡（CLAHE），只作用于 Lab 空间的 L 通道。
  kEnhanceClahe = 1,
  // 非锐化掩模锐化，强化细笔画边缘。
  kEnhanceSharpen = 2,
  // 先 CLAHE 再锐化：低对比 + 模糊的监控画面推荐。
  kEnhanceClaheSharpen = 3,
  // 双边滤波去噪：抹掉 JPEG 块噪/传感器噪点，同时保留文字边缘。
  kEnhanceDenoise = 4,
  // 先去噪再 CLAHE。
  kEnhanceDenoiseClahe = 5,
  // 白色叠加字幕提取：按「亮度高 且 饱和度低」重绘图像，把压在彩色景物
  // （树丛、广告牌、路面）上的监控字幕从背景里剥离出来。
  // 与二值化串联使用效果最好。
  kEnhanceWhiteText = 6,
};

// ---------------------------------------------------------------------------
// 通道处理方式
// ---------------------------------------------------------------------------
enum ColorMode : int32_t {
  // 保持 BGR 彩色。
  kColorKeep = 0,
  // 转灰度后再复制成 3 通道（模型仍接受 3 通道输入，但色度信息被丢弃）。
  kColorGray3 = 1,
};

// ---------------------------------------------------------------------------
// 预处理参数
//
// 执行顺序固定为：
//   缩放 → 去噪/增强 → 二值化 → 形态学 → 通道处理
// 二值化放在增强之后，是为了让自适应阈值作用在已经拉开对比的图上。
// ---------------------------------------------------------------------------
struct PreprocessParams {
  // 结构体字节大小，用于 FFI 版本协商。必须由各 Preset* 函数与
  // pp_ocr_options_init() 填写为 sizeof(PreprocessParams)。
  int32_t struct_size;

  // 缩放百分比：100 = 原始尺寸，200 = 放大一倍。
  // 放大后检测框坐标会按比例映射回原图，调用方无需关心。
  int32_t scale_percent;

  BinarizeMode binarize_mode;
  // 自适应阈值的邻域大小（像素，奇数；<=0 时按文字高度自动取）。
  int32_t adaptive_block_size;
  // 自适应阈值的常数偏移 C（越大越保守，噪点越少但易丢细字）。
  double adaptive_c;

  EnhanceMode enhance_mode;
  double clahe_clip_limit;
  int32_t clahe_grid_size;
  double sharpen_amount;

  // 形态学开运算核半径（0 = 关闭）。
  // 二值化（THRESH_BINARY_INV）后文字是「白底黑字」，而开运算 = 腐（min）
  // 后膨（max）：腐蚀让黑色扩张，因此它既能消除白色小噪点，
  // 又能把断裂的笔画连成整字 —— 这是想要「连笔」时该用的旋钮。
  int32_t morph_open_kernel;
  // 形态学闭运算核半径（0 = 关闭）。方向与开运算相反：先胀白底，
  // 因此会抹掉黑色小碎点，但同时也会吃掉「一」这类细笔画。
  int32_t morph_close_kernel;

  ColorMode color_mode;
};

// ---------------------------------------------------------------------------
// 文本检测（DB）参数
// ---------------------------------------------------------------------------
struct DetectParams {
  int32_t struct_size;

  // 概率图二值化阈值：越低越敏感（召回高、噪框多）。默认 0.30。
  double thresh;
  // 文本框平均得分阈值：低于该值的框被丢弃。默认 0.50。
  double box_thresh;
  // 文本框外扩比例。默认 1.5。
  double unclip_ratio;
  // 送入模型的输入最长边。超过则等比缩小（会损失小字细节）。
  int32_t max_side_len;
  // 最小短边（像素），小于该值的框视为噪点。
  int32_t min_box_size;
  // 检测阶段就丢弃的最低框面积（像素^2），0 = 不限制。
  int32_t min_box_area;
};

// ---------------------------------------------------------------------------
// 文本识别（CRNN + CTC）参数
// ---------------------------------------------------------------------------
struct RecognizeParams {
  int32_t struct_size;

  // 识别输入固定高度（PP-OCR 为 48）。
  int32_t img_height;
  // 识别输入的最大宽度，超宽文本按比例压缩，避免张量过大。
  int32_t max_img_width;
  // 识别前对裁剪出的文本条再跑一遍预处理（二值化/增强）：
  //   0 = 从不（保持原像素，文档类图最稳）；
  //   1 = 总是（低对比监控画面提升明显）；
  //   2 = 两种都跑并择优（最费时间，但不会因选错增强而丢字）。
  // 对「白字压绿树」这类区域，增强能显著减少漏字与错字。
  int32_t enhance_crops;
  // 文本条二次预处理使用的参数（仅当 enhance_crops != 0 时生效）。
  PreprocessParams crop_preprocess;
  // 多分支检测时，同一区域在哪些图上尝试识别：
  //   0 = 只试「实际检出该区域的分支」+ 原图；
  //   1 = 试所有分支图 + 原图（召回更高，推理次数按分支数线性增长）。
  // 放大分支往往能提供更多笔画细节，建议开 1。
  int32_t try_all_branches;
  // 丢弃低于该置信度的结果。0.0 = 全部保留。
  double min_confidence;
  // 同一区域在多个图像变体上识别时，用哪个结果胜出：
  //   0 = 只看置信度；
  //   1 = 看「字符数 × 置信度」；
  //   2 = 只看字符数（召回优先，容易把噪点识别成一串乱码）；
  //   3 = 看「识别结果的宽度与区域长宽比是否契合」；
  //   4 = 3 的基础上额外给「未二次增强的原像素候选」加分（默认）。
  // 为什么默认是 4：PP-OCR 的 CTC 置信度极易饱和（漏字也是 1.0），
  // 单靠置信度无法判断谁更全；而单靠长度会奖励「多插几个字」的乱码。
  // 区域长宽比可以直接反推大致字数（汉字占 1 个字高、ASCII 占 ≈ 0.6），
  // 拿它当标尺能同时惩罚「读少了」和「读油了」；再叠一层原像素优先，
  // 是因为二次二值化实测会偶发性地多吐/少吐几个字。
  int32_t variant_select;
};

// ---------------------------------------------------------------------------
// 检测分支（多分支召回）
//
// 单个分支 = 一种预处理 + 一套检测参数。多个分支并行检测后做框合并，
// 可以救回任何单一分支漏掉的文字。位标志可组合（按位或）。
// ---------------------------------------------------------------------------
enum BranchFlag : uint32_t {
  // 原始图，标准检测参数。任何配置都应至少包含它。
  kBranchRaw = 1u << 0,
  // 高斯自适应二值化（白字黑底 → 黑字白底）。
  kBranchAdaptiveBin = 1u << 1,
  // 全局 Otsu 二值化。
  kBranchOtsuBin = 1u << 2,
  // CLAHE 对比度增强。
  kBranchClahe = 1u << 3,
  // 2 倍放大 + 提高 max_side_len，救回极小字。
  kBranchUpscale2x = 1u << 4,
  // 更宽松的阈值组合（thresh/box_thresh 下调 + 加大外扩）。
  kBranchSensitive = 1u << 5,
  // 锐化增强。
  kBranchSharpen = 1u << 6,
  // 去噪 + CLAHE，专为高压缩监控画面。
  kBranchDenoiseClahe = 1u << 7,
  // 白色叠加字幕提取 + 自适应二值化：监控/直播画面漏检的主因就在这里。
  kBranchWhiteText = 1u << 8,
  // 3 倍放大：2x 对 20px 级小字仍会偶发繁简误读（篮→籃），3x 下识别
  // 器笔画细节充足、实测完全正确。速度比 2x 再慢一档，按需开启。
  kBranchUpscale3x = 1u << 9,

  // 常用组合。
  kBranchMono = kBranchRaw,
  kBranchAll = kBranchRaw | kBranchAdaptiveBin | kBranchOtsuBin |
               kBranchClahe | kBranchUpscale2x | kBranchSensitive |
               kBranchSharpen | kBranchDenoiseClahe | kBranchWhiteText |
               kBranchUpscale3x,
  // 监控/低对比画面推荐组合：覆盖叠加字幕与压景物的白字。
  kBranchSurveillance = kBranchRaw | kBranchAdaptiveBin | kBranchClahe |
                        kBranchUpscale2x | kBranchUpscale3x |
                        kBranchSensitive | kBranchDenoiseClahe |
                        kBranchWhiteText,
};

// ---------------------------------------------------------------------------
// 多分支合并与整行拼接参数
// ---------------------------------------------------------------------------
struct MergeParams {
  int32_t struct_size;

  // 启用哪些分支（BranchFlag 位或）。
  uint32_t branches;
  // 两个框的 IoU 超过该值即视为同一区域。
  double merge_iou_threshold;
  // 小框被大框覆盖的比例超过该值即视为重复。
  double merge_contain_threshold;
  // 允许跨分支把「同一行内水平相邻的碎框」拼成整行再识别。
  // 这是把「球场东北路口球」+「G66-57」+「24」还原为一整行的关键。
  int32_t stitch_same_line;
  // 判定同一行所需的垂直重叠比例（按较短框高度计）。
  double line_min_y_overlap;
  // 判定同一行允许的最大水平间距（相对框高的倍数）。
  // 注意这是「逐跳」间距：碎框之间通常 < 0.5 倍字高，而两段独立字幕
  // 之间的距离会达十几倍字高，所以该值必须远小于 1 才能防止跨字幕误合。
  double line_max_gap_ratio;
  // 判定同一行允许的高度差比例。
  double line_max_height_diff_ratio;
  // 拼接后如果行宽超过最长单个成员行宽的该倍数，放弃拼接（防止跨栏误合）。
  double line_max_width_ratio;
  // 最终区域外扩比例（相对短边/字高）。用于把贴边的笔画（如「一」这种
  // 单横字）纳回裁剪区。0 = 不外扩。
  double region_pad_ratio;
  // 最终区域沿「长边方向」向两侧外扩的比例（相对短边/字高）。
  // 识别裁剪窗的行向延伸：把检测框外侧紧邻的字符（典型：孤立单横「一」，
  // DB 检测模型对它没有响应）纳入识别窗 —— CRNN 识别器窗口里多一段背景
  // 不影响读出紧邻的字。监控字幕实测 1.0~1.5 即可找回行首/行尾漏检字。
  // 0 = 不延伸。
  double region_pad_long_ratio;
  // 最终结果数量上限，0 = 不限制。
  int32_t max_regions;
  // 排序方式：0 = 阅读顺序（先上后左），1 = 面积降序，2 = 分支产出顺序。
  int32_t sort_order;
};

// ---------------------------------------------------------------------------
// 完整管道选项
// ---------------------------------------------------------------------------
struct OcrOptions {
  int32_t struct_size;

  DetectParams det;
  RecognizeParams rec;
  MergeParams merge;

  // 感兴趣区域：只识别该矩形内的文字，结果坐标仍以「原图」为基准。
  // 全 0 表示整图。适合只要时间戳条 / 只要某个车牌区域的场景。
  // 注意：区域太小时检测器会失去上下文，建议沿文字行方向多留一些边。
  int32_t roi_x;
  int32_t roi_y;
  int32_t roi_w;
  int32_t roi_h;

  // 记录每个分支的检出数量到引擎日志，便于调参。0 = 关闭。
  int32_t verbose_stats;
};

// ---------------------------------------------------------------------------
// 预设
//
// 所有预设都以「单分支标准流程」为基线，只在召回策略上做增量，
// 因此切换预设不会改变已有调用方的坐标语义。
//
// 历史上的 standard / document / lowContrast / smallText 已删除：
// 实测它们对监控字幕类图片（如「一舍篮球场东北路口球G66-5724」）无法
// 读出完整内容。编号沿用发布时的取值，不重排，中间留空洞。
// ---------------------------------------------------------------------------
enum OptionsPreset : int32_t {
  kPresetSurveillance = 3,  // 监控画面：叠加字幕 + 复杂景物
  kPresetMaxRecall = 5,     // 全分支 + 全部后处理，最慢但最全
};

// 用「标准」默认值填充一个选项结构（各字段的唯一权威定义处）。
void FillDefaultPreprocess(PreprocessParams* p);
void FillDefaultDetect(DetectParams* p);
void FillDefaultRecognize(RecognizeParams* p);
void FillDefaultMerge(MergeParams* p);
void FillDefaultOptions(OcrOptions* o);

// 按预设生成选项。未知预设等价于标准默认值（单分支旧行为）。
OcrOptions MakePresetOptions(int32_t preset);

// 校验并原地修正非法取值（例如 block_size 取偶数、负阈值）。
// 返回被修正的字段数量，0 表示无需修正。
int32_t SanitizeOptions(OcrOptions* o);

// 预设 / 分支的可读名称，仅用于日志与调试接口。
const char* PresetName(int32_t preset);
std::string BranchFlagsName(uint32_t branches);

// ---------------------------------------------------------------------------
// 从宿主传来的「键 → 数值」映射还原选项（MethodChannel 回退路径用）
//
// Linux / Windows 两边的 Encodable 类型不同，但「按名字取一个数」这件事
// 同构。把 40 个字段的名单收敛到这里一份，避免两个平台各维护一份而漏项。
// ---------------------------------------------------------------------------
struct MapLookup {
  // 键不存在或类型不对时，必须返回 fallback。
  std::function<double(const char* key, double fallback)> get_double;
  std::function<int32_t(const char* key, int32_t fallback)> get_int;
};

// 按 lookup 还原一份选项（已知键全覆盖，未知键忽略），并自动 sanitize。
OcrOptions OptionsFromMapLookup(const MapLookup& lookup);

}  // namespace paddle_ocr

#endif  // OCR_OPTIONS_H_
