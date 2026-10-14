#ifndef OCR_PREPROCESS_H_
#define OCR_PREPROCESS_H_

#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "ocr_options.h"

namespace paddle_ocr {

// ===========================================================================
// 图像预处理算子
//
// 全部为无状态自由函数：输入 Mat、输出 Mat，便于单元测试，也便于宿主
// 只借用其中某一个算子（例如只做二值化）而不走整条 OCR 管道。
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
  // 「参考几何」：在 region_pad_ratio 外扩**之前**的长短边。
  // 外扩后的短边包含了空白，拿它反推预期字数会系统性低估，
  // 从而惩罚掉读得最全的候选，所以这里单独存一份。
  double ref_long_side = 0.0;
  double ref_short_side = 0.0;
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
