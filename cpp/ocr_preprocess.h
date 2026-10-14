#ifndef OCR_PREPROCESS_H_
#define OCR_PREPROCESS_H_

#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "ocr_options.h"

namespace paddle_ocr {

// ===========================================================================
// 预处理插件接口（IImageTransform）
//
// 预处理管线由有序的「变换步骤」组成。每个步骤实现该接口；宿主（嵌入
// 引擎的其他 App）可以实现自己的步骤并组装进 PreprocessPipeline，从而
// 注入任意的图像预处理逻辑（色彩域提取、超分、去雾……），而无需改动
// 引擎源码。内置步骤（缩放/增强/二值化/形态学/通道）同样实现该接口。
//
// 线程约定：Apply 必须是 const 且无状态（同一实例可被并发调用）。
// ===========================================================================
class IImageTransform {
 public:
  virtual ~IImageTransform() = default;
  // 步骤名，用于日志与诊断。
  virtual const char* name() const = 0;
  // 对输入图做变换并返回新图。p 是整条管线共享的参数包，各步骤只读
  // 自己关心的字段。
  virtual cv::Mat Apply(const cv::Mat& src, const PreprocessParams& p) const = 0;
};

// ---------------------------------------------------------------------------
// 内置变换步骤（与历史 ApplyXxx 自由函数一一对应）
// ---------------------------------------------------------------------------

// 步骤 1：按 p.scale_percent 缩放。
class ScaleTransform : public IImageTransform {
 public:
  const char* name() const override { return "scale"; }
  cv::Mat Apply(const cv::Mat& src, const PreprocessParams& p) const override;
};

// 步骤 2：按 p.enhance_mode 做去噪/增强/白字提取。
class EnhanceTransform : public IImageTransform {
 public:
  const char* name() const override { return "enhance"; }
  cv::Mat Apply(const cv::Mat& src, const PreprocessParams& p) const override;
};

// 步骤 3：按 p.binarize_mode 二值化。
class BinarizeTransform : public IImageTransform {
 public:
  const char* name() const override { return "binarize"; }
  cv::Mat Apply(const cv::Mat& src, const PreprocessParams& p) const override;
};

// 步骤 4：按 p.morph_*_kernel 做形态学开闭。
class MorphologyTransform : public IImageTransform {
 public:
  const char* name() const override { return "morphology"; }
  cv::Mat Apply(const cv::Mat& src, const PreprocessParams& p) const override;
};

// 步骤 5：按 p.color_mode 做通道处理。
class ColorTransform : public IImageTransform {
 public:
  const char* name() const override { return "color"; }
  cv::Mat Apply(const cv::Mat& src, const PreprocessParams& p) const override;
};

// ---------------------------------------------------------------------------
// 预处理管线：有序步骤列表。
//
// 用法一（默认管线）= 历史 ApplyPreprocess 的行为：
//   const PreprocessPipeline& pipeline = DefaultPreprocessPipeline();
//   cv::Mat out = pipeline.Run(src, params, &applied_scale);
//
// 用法二（宿主自定义）：清空后按需组装，甚至插入自己的变换：
//   PreprocessPipeline pipeline;
//   pipeline.AddStep(std::make_shared<MySuperresTransform>());
//   pipeline.AddStep(std::make_shared<BinarizeTransform>());
// ===========================================================================
class PreprocessPipeline {
 public:
  // 添加一个步骤（shared_ptr 便于宿主复用同一实例）。
  PreprocessPipeline& AddStep(std::shared_ptr<IImageTransform> step);
  // 重置为内置默认顺序（scale→enhance→binarize→morphology→color）。
  void UseDefaults();
  // 清空所有步骤。空管线 = 原样返回。
  void Clear();
  bool Empty() const { return steps_.empty(); }
  // 步骤名列表（日志用）。
  std::vector<std::string> StepNames() const;
  // 依次执行。applied_scale 回填管线内实际发生的累计缩放（仅当默认
  // 管线或第一步是缩放时有意义；自定义管线请自行处理坐标换算）。
  cv::Mat Run(const cv::Mat& src, const PreprocessParams& p,
              double* applied_scale = nullptr) const;

 private:
  std::vector<std::shared_ptr<IImageTransform>> steps_;
};

// 取默认管线的共享实例（只读，线程安全）。
const PreprocessPipeline& DefaultPreprocessPipeline();

// ===========================================================================
// 兼容的自由函数入口（沿用历史语义，内部走内置算子）
// ===========================================================================

// 按完整参数依次执行：缩放 → 增强 → 二值化 → 形态学 → 通道处理。
// applied_scale 回填实际缩放系数（供检测框坐标反算回原图）。
cv::Mat ApplyPreprocess(const cv::Mat& src, const PreprocessParams& p,
                        double* applied_scale = nullptr);

// 单步算子（也可独立调用）。
cv::Mat ApplyScale(const cv::Mat& src, int scale_percent,
                   double* applied_scale = nullptr);
cv::Mat ApplyEnhance(const cv::Mat& src, const PreprocessParams& p);
cv::Mat ApplyBinarize(const cv::Mat& src, const PreprocessParams& p);
cv::Mat ApplyMorphology(const cv::Mat& src, const PreprocessParams& p);
cv::Mat ApplyColorMode(const cv::Mat& src, ColorMode mode);

// 对裁剪出的文本条做二次预处理。与整图预处理的差别在于：块大小等参数
// 会按文本条实际高度自动收敛（48px 高的条用 31 邻域会把字吃掉）。
cv::Mat PrepareCropForRecognition(const cv::Mat& crop,
                                  const PreprocessParams& params);

// 自动判断文字极性：返回 true 表示「文字比局部背景更亮」，
// 此时应使用 THRESH_BINARY_INV 把文字压成黑字白底。
bool DetectTextIsBright(const cv::Mat& gray);

// ===========================================================================
// 文本框几何
// ===========================================================================

// 一个检测框 + 它的来源信息。坐标一律在「原始输入图」坐标系下。
struct DetectedBox {
  std::vector<cv::Point> points;  // 4 角点
  int branch_index = 0;           // 产生该框的分支下标
  double score = 0.0;             // 分支内的 DB 平均得分
};

// 合并后的区域：可能由多个分支的多个框共同贡献。
struct MergedBox {
  std::vector<cv::Point> points;
  std::vector<int> branches;  // 升序、去重
  double score = 0.0;         // 贡献框得分的最大值
  int member_count = 1;       // 参与合并的原始框数量
  bool stitched = false;      // 是否由「同行碎框拼接」而来
  // 成员框（原图坐标系）。union 会把「贴边碎片」的歪斜几何一并吃进来
  //（典型：图像顶部的残框把 union 撑高、拉宽），直接拿 union 当识别窗
  // 会稀释字带、引入错字；识别窗应优先用最贴合字带的成员框。
  std::vector<std::vector<cv::Point>> member_boxes;
  // 「参考几何」：在 region_pad_ratio 外扩**之前**的长短边。
  // 外扩后的短边包含了空白，拿它反推预期字数会系统性低估，
  // 从而惩罚掉读得最全的候选，所以这里单独存一份。
  double ref_long_side = 0.0;
  double ref_short_side = 0.0;
  // 「字高」= 成员框短边的中位数（merge 阶段统计）。union 的短边会被
  // 贴边碎片撑大一倍，用它反推预期字数会把「读少了」的候选误判为
  // 「贴合」；真实字高才是可靠的标尺。0 = 未知（退回 ref_short_side）。
  double text_height = 0.0;
};

double BoxArea(const std::vector<cv::Point>& box);
cv::Rect BoxBoundingRect(const std::vector<cv::Point>& box);
cv::Point2f BoxCenter(const std::vector<cv::Point>& box);
// 短边长度（≈文字高度）与长边长度（≈行宽）。
double BoxShortSide(const std::vector<cv::Point>& box);
double BoxLongSide(const std::vector<cv::Point>& box);
// 长边与水平线的夹角（度，0~90）。
double BoxAngle(const std::vector<cv::Point>& box);

// 旋转矩形意义下的交面积 / IoU / 包含度。退化输入返回 0。
double BoxIntersectionArea(const std::vector<cv::Point>& a,
                           const std::vector<cv::Point>& b);
double BoxIoU(const std::vector<cv::Point>& a, const std::vector<cv::Point>& b);
// inner 被 outer 覆盖的面积比例（0~1）。
double BoxContainment(const std::vector<cv::Point>& inner,
                      const std::vector<cv::Point>& outer);

// 把若干框合成一个尽量贴合的最小旋转矩形（返回 4 角点）。
std::vector<cv::Point> UnionBoxes(
    const std::vector<std::vector<cv::Point>>& boxes);

// 判断两个框是否应视为同一区域。
// 三条判据（满足任一即重复）：
//   1. IoU ≥ merge_iou_threshold；
//   2. 包含度 ≥ merge_contain_threshold；
//   3. 「同一水平带 + 水平重叠」：两框中心 y 差 < 0.6×平均字高 且
//      水平方向重叠长度 > 0.7×较短框宽。文本行是水平延伸目标，跨分支
//      检出的行碎片往往垂直对不齐（贴边框 y 从 0 开始），用 IoU/包含度
//      会漏判 —— 典型残留如整行「…G66-5724」旁多出的「724」碎框。
bool BoxesAreDuplicate(const std::vector<cv::Point>& a,
                       const std::vector<cv::Point>& b, const MergeParams& m);

// ===========================================================================
// 多分支结果合并
// ===========================================================================

// 跨分支去重合并：把不同分支检出的同一区域并成一个框，
// 并记录所有贡献分支，供后续「择优识别」使用。
std::vector<MergedBox> MergeDetectedBoxes(
    const std::vector<DetectedBox>& boxes, const MergeParams& m);

// 同行碎框拼接：把同一文本行内水平相邻的碎框合成整行。
// 这是把「球场东北路口球」+「G66-57」+「24」还原成完整一行的关键步骤。
std::vector<MergedBox> StitchSameLine(const std::vector<MergedBox>& boxes,
                                      const MergeParams& m);

// 把每个区域沿长、短边各外扩 `pad_ratio * 短边`，并夹到图像范围内。
// 用于把贴边笔画（如「一」这种单横字）纳回裁剪区。
void PadRegions(std::vector<MergedBox>* regions, double pad_ratio,
                int image_width, int image_height);

// 把单个框沿「长边方向」向两侧各外扩 `pad_long_ratio * 短边`（字高），
// 短边方向不动，结果夹回图像范围。PadRegionsAlongLongSide 的单框形式。
std::vector<cv::Point> PadBoxAlongLongSide(const std::vector<cv::Point>& box,
                                           double pad_long_ratio,
                                           int image_width, int image_height);

// 把每个区域沿「长边方向」向两侧各外扩 `pad_long_ratio * 短边`（字高），
// 垂直（短边）方向不动。用于把**检测框外侧紧邻的字符**（典型：孤立单横
// 「一」，DB 检测模型对它没有响应）纳入识别裁剪窗 —— 识别器是整行
// CRNN，窗口里多一段背景不会影响它读出紧邻的字。
// 实测：对 22px 字高的监控字幕行，外扩 1.0 倍字高即可把行首「一」找回。
void PadRegionsAlongLongSide(std::vector<MergedBox>* regions,
                             double pad_long_ratio, int image_width,
                             int image_height);

// 把单个框四周均匀外扩 `pad_ratio * 短边`，夹回图像范围。
// PadRegions 的单框形式。
std::vector<cv::Point> PadBox(const std::vector<cv::Point>& box,
                              double pad_ratio, int image_width,
                              int image_height);

// 为一个合并区域构造「识别窗」：
//   1. 基底优先取成员框中长边最大者（最贴合字带的整行检出）；
//      只有当它不足区域长边的 anchor_min_region_ratio 时（成员全是
//      碎框）才退回区域自身。
//   2. 基底先四周外扩 pad_ratio × 短边（把贴边笔画纳回），再沿长边
//      方向向两侧各外扩 pad_long_ratio × 短边（找回紧邻的漏检字），
//      每步都夹回图像范围。
// 返回 4 角点（旋转矩形，坐标在原图系）。识别窗独立于 region.points，
// 供择优识别与宿主编排使用。
std::vector<cv::Point> BuildRecognitionWindow(
    const MergedBox& region, double pad_ratio, double pad_long_ratio,
    int image_width, int image_height,
    double anchor_min_region_ratio = 0.7);

// 排序：0 = 阅读顺序（先上后左，按行分组），1 = 面积降序，2 = 保持入参顺序。
void SortBoxesForReadingOrder(std::vector<MergedBox>* boxes, int sort_order);

// ===========================================================================
// 多变体择优
// ===========================================================================

// 估算一段文本按「字高」为单位的排版宽度：CJK 记 1.0，ASCII 字母数字
// 记 0.6，标点空格记 0.35，其余记 0.7。用于把区域长宽比换算成预期字数。
double EstimateTextWidthInEm(const std::string& utf8_text);

// 同一区域在多个图像变体上识别后，给单个候选结果打分（越大越优）。
// mode 取 RecognizeParams::variant_select；long_side / short_side
// 是该区域在原图里的长短边像素数（用 ref_* 那组未外扩的值）；
// raw_crop 表示本次候选是否直接拿原像素文本条去识别（未二次增强）。
double VariantSelectionScore(int32_t mode, const std::string& text,
                             float confidence, double long_side,
                             double short_side, bool raw_crop);

}  // namespace paddle_ocr

#endif  // OCR_PREPROCESS_H_
