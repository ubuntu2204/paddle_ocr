// Shared C++ unit tests for the pp_ocr plugin.
//
// These tests exercise cross-platform code (cpp/debug_utils.h and
// cpp/pp_ocr_ffi.h/cpp) that is compiled into both the Windows and Linux
// plugin binaries. They are linked into each platform's test runner via
// windows/CMakeLists.txt and linux/CMakeLists.txt.
//
// Coverage:
//   * debug_utils::HexDump        – ASCII, high-byte, empty, oversized input
//   * debug_utils::ValidateUtf8Detailed – valid / truncated / bad lead byte
//                                       / overlong / surrogate / > U+10FFFF
//   * pp_ocr_ffi C API            – create/destroy/version and null-safety
//                                   behavior on every entrypoint
#include <gtest/gtest.h>

#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "debug_utils.h"
#include "pp_ocr_ffi.h"

#include "ocr_engine.h"
#include "ocr_options.h"
#include "ocr_preprocess.h"

// OpenCV 仅用于在集成测试里合成一张带文字的图片，无额外依赖。
#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace paddle_ocr {
namespace test {

// ===========================================================================
// debug_utils.h – HexDump
// ===========================================================================

TEST(HexDump, EmptyStringProducesEmptyOutput) {
  EXPECT_EQ(HexDump(std::string()), "");
}

TEST(HexDump, AsciiBytesAreUppercaseTwoDigitHex) {
  std::string s = "AB";  // 0x41 0x42
  EXPECT_EQ(HexDump(s), "41 42");
}

TEST(HexDump, HighBytesAndControlChars) {
  std::string s;
  s.push_back(static_cast<char>(0x00));
  s.push_back(static_cast<char>(0x0F));
  s.push_back(static_cast<char>(0xC3));
  s.push_back(static_cast<char>(0x28));
  EXPECT_EQ(HexDump(s), "00 0F C3 28");
}

TEST(HexDump, TruncatesAtMaxBytesAndReportsTotal) {
  std::string s(200, 'A');
  std::string out = HexDump(s, /*max_bytes=*/4);
  EXPECT_EQ(out, "41 41 41 41 ... (200 bytes total)");
}

TEST(HexDump, DefaultLimitIs128Bytes) {
  std::string s(200, 'A');
  std::string out = HexDump(s);
  // 128 hex pairs + a truncation suffix mentioning 200 bytes total.
  EXPECT_NE(out.find("... (200 bytes total)"), std::string::npos);
  // The first 128 hex pairs (each "41 ") should be present.
  size_t spaces = 0;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i] == ' ') ++spaces;
    if (out[i] == '.') break;
  }
  // 127 spaces between hex pairs + 1 space before "..." = 128.
  EXPECT_EQ(spaces, 128u);
}

// ===========================================================================
// debug_utils.h – ValidateUtf8Detailed
// ===========================================================================

TEST(ValidateUtf8, EmptyStringIsValid) {
  EXPECT_EQ(ValidateUtf8Detailed(std::string()), "");
}

TEST(ValidateUtf8, AsciiIsValid) {
  EXPECT_EQ(ValidateUtf8Detailed("Hello, world! 0123456789"), "");
}

TEST(ValidateUtf8, Valid2ByteSequence) {
  // U+00E9 é = C3 A9
  std::string s;
  s.push_back(static_cast<char>(0xC3));
  s.push_back(static_cast<char>(0xA9));
  EXPECT_EQ(ValidateUtf8Detailed(s), "");
}

TEST(ValidateUtf8, Valid3ByteSequence) {
  // 汉字 "中" U+4E2D = E4 B8 AD
  std::string s;
  s.push_back(static_cast<char>(0xE4));
  s.push_back(static_cast<char>(0xB8));
  s.push_back(static_cast<char>(0xAD));
  EXPECT_EQ(ValidateUtf8Detailed(s), "");
}

TEST(ValidateUtf8, Valid4ByteSequence) {
  // U+1F680 🚀 = F0 9F 98 80
  std::string s;
  s.push_back(static_cast<char>(0xF0));
  s.push_back(static_cast<char>(0x9F));
  s.push_back(static_cast<char>(0x98));
  s.push_back(static_cast<char>(0x80));
  EXPECT_EQ(ValidateUtf8Detailed(s), "");
}

TEST(ValidateUtf8, RejectsInvalidLeadByte) {
  std::string s;
  s.push_back(static_cast<char>(0xFF));
  std::string err = ValidateUtf8Detailed(s);
  EXPECT_NE(err.find("not a valid UTF-8 start byte"), std::string::npos);
}

TEST(ValidateUtf8, RejectsTruncatedSequence) {
  std::string s;
  s.push_back(static_cast<char>(0xE4));  // start of 3-byte
  s.push_back(static_cast<char>(0xB8));  // one continuation
  // missing third byte
  std::string err = ValidateUtf8Detailed(s);
  EXPECT_NE(err.find("truncated"), std::string::npos);
}

TEST(ValidateUtf8, RejectsBadContinuationByte) {
  std::string s;
  s.push_back(static_cast<char>(0xE4));
  s.push_back(static_cast<char>(0xB8));
  s.push_back(static_cast<char>(0x2D));  // not 0x80..0xBF
  std::string err = ValidateUtf8Detailed(s);
  EXPECT_NE(err.find("expected continuation byte"), std::string::npos);
}

TEST(ValidateUtf8, RejectsOverlong2Byte) {
  // C1 80 encodes U+0000, which is overlong.
  std::string s;
  s.push_back(static_cast<char>(0xC1));
  s.push_back(static_cast<char>(0x80));
  std::string err = ValidateUtf8Detailed(s);
  EXPECT_NE(err.find("overlong"), std::string::npos);
}

TEST(ValidateUtf8, RejectsOverlong3Byte) {
  // E0 80 80 encodes < 0x800, overlong.
  std::string s;
  s.push_back(static_cast<char>(0xE0));
  s.push_back(static_cast<char>(0x80));
  s.push_back(static_cast<char>(0x80));
  std::string err = ValidateUtf8Detailed(s);
  EXPECT_NE(err.find("overlong"), std::string::npos);
}

TEST(ValidateUtf8, RejectsSurrogateHalf) {
  // U+D800 in "UTF-8" is ED A0 80 (invalid per spec).
  std::string s;
  s.push_back(static_cast<char>(0xED));
  s.push_back(static_cast<char>(0xA0));
  s.push_back(static_cast<char>(0x80));
  std::string err = ValidateUtf8Detailed(s);
  EXPECT_NE(err.find("surrogate"), std::string::npos);
}

TEST(ValidateUtf8, RejectsCodepointAbove10FFFF) {
  // U+110000 → F4 90 80 80 (exceeds U+10FFFF).
  std::string s;
  s.push_back(static_cast<char>(0xF4));
  s.push_back(static_cast<char>(0x90));
  s.push_back(static_cast<char>(0x80));
  s.push_back(static_cast<char>(0x80));
  std::string err = ValidateUtf8Detailed(s);
  EXPECT_NE(err.find("exceeds U+10FFFF"), std::string::npos);
}

TEST(ValidateUtf8, MixedValidAsciiAndMultibyte) {
  std::string s = "a中b🚀c";
  EXPECT_EQ(ValidateUtf8Detailed(s), "");
}

// ===========================================================================
// pp_ocr_ffi C API
// ===========================================================================

class FfiApiTest : public ::testing::Test {
 protected:
  PpOcrEngine* engine_ = nullptr;

  void SetUp() override { engine_ = pp_ocr_create(); }
  void TearDown() override {
    if (engine_) pp_ocr_destroy(engine_);
    engine_ = nullptr;
  }
};

TEST(FfiVersion, ReturnsNonEmptyStaticString) {
  const char* v = pp_ocr_version();
  ASSERT_NE(v, nullptr);
  EXPECT_GT(std::strlen(v), 0u);
  EXPECT_NE(std::string(v).find("pp_ocr"), std::string::npos);
}

TEST(FfiDestroy, NullIsSafe) {
  // pp_ocr_destroy explicitly documents that passing nullptr is safe.
  pp_ocr_destroy(nullptr);
}

TEST_F(FfiApiTest, CreateReturnsNonNull) { EXPECT_NE(engine_, nullptr); }

TEST_F(FfiApiTest, IsInitializedFalseByDefault) {
  EXPECT_EQ(pp_ocr_is_initialized(engine_), 0);
}

TEST_F(FfiApiTest, IsInitializedNullReturnsFalse) {
  EXPECT_EQ(pp_ocr_is_initialized(nullptr), 0);
}

TEST_F(FfiApiTest, GetLastErrorNullReturnsNull) {
  EXPECT_EQ(pp_ocr_get_last_error(nullptr), nullptr);
}

TEST_F(FfiApiTest, GetLastErrorFreshEngineReturnsNull) {
  // No error yet on a fresh engine.
  EXPECT_EQ(pp_ocr_get_last_error(engine_), nullptr);
}

TEST_F(FfiApiTest, InitializeRejectsNullEngine) {
  EXPECT_EQ(pp_ocr_initialize(nullptr, "a", "b", "c"), 0);
}

TEST_F(FfiApiTest, InitializeRejectsNullPaths) {
  EXPECT_EQ(pp_ocr_initialize(engine_, nullptr, "b", "c"), 0);
  EXPECT_EQ(pp_ocr_initialize(engine_, "a", nullptr, "c"), 0);
  EXPECT_EQ(pp_ocr_initialize(engine_, "a", "b", nullptr), 0);
}

TEST_F(FfiApiTest, InitializeFailsWithNonexistentPathsAndSetsError) {
  int ok = pp_ocr_initialize(engine_,
                              "/__does_not_exist__/det.onnx",
                              "/__does_not_exist__/rec.onnx",
                              "/__does_not_exist__/dict.txt");
  EXPECT_EQ(ok, 0);
  const char* err = pp_ocr_get_last_error(engine_);
  EXPECT_NE(err, nullptr);
}

TEST_F(FfiApiTest, RecognizeFileRejectsNullEngine) {
  PpOcrResultArray r = pp_ocr_recognize_file(nullptr, "/tmp/x.png");
  EXPECT_NE(r.error, nullptr);
  EXPECT_EQ(r.items, nullptr);
  EXPECT_EQ(r.count, 0);
}

TEST_F(FfiApiTest, RecognizeFileRejectsNullPath) {
  PpOcrResultArray r = pp_ocr_recognize_file(engine_, nullptr);
  EXPECT_NE(r.error, nullptr);
}

TEST_F(FfiApiTest, RecognizeFileRequiresInitializedEngine) {
  PpOcrResultArray r = pp_ocr_recognize_file(engine_, "/tmp/x.png");
  ASSERT_NE(r.error, nullptr);
  EXPECT_NE(std::string(r.error).find("not initialized"), std::string::npos);
}

TEST_F(FfiApiTest, RecognizeBytesRejectsNullEngine) {
  uint8_t buf[4] = {1, 2, 3, 4};
  PpOcrResultArray r = pp_ocr_recognize_bytes(nullptr, buf, 4);
  EXPECT_NE(r.error, nullptr);
}

TEST_F(FfiApiTest, RecognizeBytesRejectsNullData) {
  PpOcrResultArray r = pp_ocr_recognize_bytes(engine_, nullptr, 4);
  EXPECT_NE(r.error, nullptr);
}

TEST_F(FfiApiTest, RecognizeBytesRejectsZeroLength) {
  uint8_t buf[1] = {0};
  PpOcrResultArray r = pp_ocr_recognize_bytes(engine_, buf, 0);
  EXPECT_NE(r.error, nullptr);
}

TEST_F(FfiApiTest, RecognizeBytesRejectsNegativeLength) {
  uint8_t buf[1] = {0};
  PpOcrResultArray r = pp_ocr_recognize_bytes(engine_, buf, -1);
  EXPECT_NE(r.error, nullptr);
}

TEST_F(FfiApiTest, RecognizeBytesRequiresInitializedEngine) {
  uint8_t buf[4] = {1, 2, 3, 4};
  PpOcrResultArray r = pp_ocr_recognize_bytes(engine_, buf, 4);
  ASSERT_NE(r.error, nullptr);
  EXPECT_NE(std::string(r.error).find("not initialized"), std::string::npos);
}

TEST_F(FfiApiTest, CreateDestroyCycleAllowsReinit) {
  // Ensure calling destroy() and then create() again returns a working
  // pointer with no state leakage.
  PpOcrEngine* e2 = pp_ocr_create();
  ASSERT_NE(e2, nullptr);
  EXPECT_EQ(pp_ocr_is_initialized(e2), 0);
  pp_ocr_destroy(e2);
}

// ===========================================================================
// ocr_options.h / .cpp —— 默认值、预设、合法性修正
// ===========================================================================
namespace {

// 分支掩码里的位数。
int CountBranches(uint32_t mask) {
  int n = 0;
  for (uint32_t b = kBranchRaw; b != 0; b <<= 1) {
    if (mask & b) ++n;
  }
  return n;
}

std::vector<cv::Point> MakeRect(int x, int y, int w, int h) {
  return {cv::Point(x, y), cv::Point(x + w, y), cv::Point(x + w, y + h),
          cv::Point(x, y + h)};
}

// 造一个已合并区域，参数与 MergeDetectedBoxes 的输出保持一致。
MergedBox MakeMerged(const std::vector<cv::Point>& pts,
                     const std::vector<int>& branches, double score) {
  MergedBox mb;
  mb.points = pts;
  mb.branches = branches;
  mb.score = score;
  mb.member_count = 1;
  mb.stitched = false;
  mb.ref_long_side = BoxLongSide(pts);
  mb.ref_short_side = BoxShortSide(pts);
  return mb;
}

}  // namespace

TEST(OcrOptions, DefaultsMatchHistoricalHardcodedValues) {
  OcrOptions o;
  FillDefaultOptions(&o);
  // 这三个值是旧版本写死在 TextDetector 里的，改了就会改变不传选项时的行为
  EXPECT_DOUBLE_EQ(o.det.thresh, 0.30);
  EXPECT_DOUBLE_EQ(o.det.box_thresh, 0.50);
  EXPECT_DOUBLE_EQ(o.det.unclip_ratio, 1.5);
  EXPECT_EQ(o.det.max_side_len, 960);
  EXPECT_EQ(o.merge.branches, static_cast<uint32_t>(kBranchMono));
  EXPECT_EQ(o.merge.stitch_same_line, 0);
  EXPECT_EQ(o.rec.enhance_crops, 0);
  EXPECT_EQ(SanitizeOptions(&o), 0) << "默认值本身必须全部合法";
}

TEST(OcrOptions, PresetsAreStrictlyWiderThanStandard) {
  OcrOptions standard;
  FillDefaultOptions(&standard);
  // 旧 standard/document/lowContrast/smallText 已删除，编号留空洞不重排。
  for (int32_t p : {kPresetSurveillance, kPresetMaxRecall}) {
    const OcrOptions o = MakePresetOptions(p);
    EXPECT_GT(CountBranches(o.merge.branches), 1)
        << "preset " << PresetName(p) << " 应该开多分支";
    EXPECT_GE(o.merge.branches & kBranchRaw, kBranchRaw)
        << "任何预设都应保留原图分支作为基线";
    EXPECT_GE(o.det.max_side_len, standard.det.max_side_len);
  }
}

TEST(OcrOptions, SurveillancePresetEnablesTheFixesThisRepoNeeds) {
  const OcrOptions o = MakePresetOptions(kPresetSurveillance);
  // 白字提取 + 同行拼接 + 文本条择优，是「监控字幕识别不全」的三件套
  EXPECT_NE(o.merge.branches & kBranchWhiteText, 0u);
  EXPECT_EQ(o.merge.stitch_same_line, 1);
  EXPECT_EQ(o.rec.enhance_crops, 2);
  EXPECT_GT(o.merge.region_pad_ratio, 0.0);
  EXPECT_EQ(o.rec.variant_select, 4);
}

TEST(OcrOptions, SanitizeRepairsIllegalValues) {
  OcrOptions o;
  FillDefaultOptions(&o);
  o.det.thresh = 42.0;          // 越界
  o.det.max_side_len = -5;      // 非法
  o.merge.branches = 0;         // 至少要留一路
  o.merge.line_max_gap_ratio = -1;
  o.rec.enhance_crops = 99;     // 三态之外
  o.rec.crop_preprocess.adaptive_block_size = 30;  // 偶数
  o.roi_w = -100;               // 负宽高
  const int32_t fixed = SanitizeOptions(&o);
  EXPECT_GE(fixed, 7);
  EXPECT_DOUBLE_EQ(o.det.thresh, 0.30);
  EXPECT_EQ(o.det.max_side_len, 960);
  EXPECT_EQ(o.merge.branches, static_cast<uint32_t>(kBranchRaw));
  EXPECT_GT(o.merge.line_max_gap_ratio, 0.0);
  EXPECT_LE(o.rec.enhance_crops, 2);
  EXPECT_EQ(o.rec.crop_preprocess.adaptive_block_size % 2, 1);
  EXPECT_EQ(o.roi_w, 0);
}

TEST(OcrOptions, SanitizeDropsUnknownBranchBits) {
  OcrOptions o;
  FillDefaultOptions(&o);
  o.merge.branches = kBranchRaw | 0xFFFF0000u;  // 高位未定义
  SanitizeOptions(&o);
  EXPECT_EQ(o.merge.branches, static_cast<uint32_t>(kBranchRaw));
}

TEST(OcrOptions, BranchFlagsNameListsEveryFlag) {
  const std::string name = BranchFlagsName(kBranchRaw | kBranchWhiteText);
  EXPECT_NE(name.find("raw"), std::string::npos);
  EXPECT_NE(name.find("white_text"), std::string::npos);
  EXPECT_EQ(BranchFlagsName(0), "(none)");
}

TEST(OcrOptions, OptionsFromMapLookupReadsEveryField) {
  // MethodChannel 回退路径用的解析器：关键几个字段必须能落到结构里
  MapLookup lk;
  lk.get_double = [](const char* key, double fb) {
    if (std::string(key) == "detThresh") return 0.21;
    if (std::string(key) == "regionPadRatio") return 0.4;
    return fb;
  };
  lk.get_int = [](const char* key, int32_t fb) {
    if (std::string(key) == "branches") return static_cast<int32_t>(kBranchAll);
    if (std::string(key) == "stitchSameLine") return 1;
    return fb;
  };
  const OcrOptions o = OptionsFromMapLookup(lk);
  EXPECT_DOUBLE_EQ(o.det.thresh, 0.21);
  EXPECT_DOUBLE_EQ(o.merge.region_pad_ratio, 0.4);
  EXPECT_EQ(o.merge.branches, static_cast<uint32_t>(kBranchAll));
  EXPECT_EQ(o.merge.stitch_same_line, 1);
  // 未提供的键保持默认
  EXPECT_DOUBLE_EQ(o.det.box_thresh, 0.50);
}

// ===========================================================================
// ocr_preprocess.cpp —— 图像算子
// ===========================================================================
namespace {

// 造一张「绿色噪声背景 + 白色叠加字幕」的图，复现监控画面的漏检场景。
cv::Mat MakeWhiteOnNoiseImage(int width = 640, int height = 160) {
  cv::Mat img(height, width, CV_8UC3);
  cv::RNG rng(12345);
  for (int y = 0; y < height; ++y) {
    auto* row = img.ptr<cv::Vec3b>(y);
    for (int x = 0; x < width; ++x) {
      // 高饱和的绿色底噪：让灰度图的分隔度极低
      row[x] = cv::Vec3b(static_cast<uchar>(rng.uniform(10, 70)),
                         static_cast<uchar>(rng.uniform(90, 200)),
                         static_cast<uchar>(rng.uniform(10, 70)));
    }
  }
  cv::putText(img, "HELLO12345", cv::Point(40, height - 55),
              cv::FONT_HERSHEY_SIMPLEX, /*scale=*/1.6,
              cv::Scalar(250, 250, 250), /*thickness=*/3);
  return img;
}

}  // namespace

TEST(OcrPreprocess, ApplyScaleReportsAndHonoursPercent) {
  cv::Mat src = cv::Mat::zeros(40, 100, CV_8UC3);
  double scale = 0;
  cv::Mat up = ApplyScale(src, 200, &scale);
  EXPECT_EQ(up.rows, 80);
  EXPECT_EQ(up.cols, 200);
  EXPECT_DOUBLE_EQ(scale, 2.0);

  double down = 0;
  cv::Mat small = ApplyScale(src, 50, &down);
  EXPECT_EQ(small.rows, 20);
  EXPECT_EQ(small.cols, 50);
  EXPECT_DOUBLE_EQ(down, 0.5);

  // 非法百分比不能把图搞没
  cv::Mat same = ApplyScale(src, 0);
  EXPECT_EQ(same.rows, 40);
  EXPECT_EQ(same.cols, 100);
}

TEST(OcrPreprocess, BinarizeProducesPureBlackWhiteThreeChannel) {
  cv::Mat src = MakeWhiteOnNoiseImage();
  PreprocessParams p;
  FillDefaultPreprocess(&p);
  p.binarize_mode = kBinarizeAdaptiveGaussInv;

  cv::Mat out = ApplyBinarize(src, p);
  ASSERT_EQ(out.channels(), 3);  // 模型固定吃 3 通道
  ASSERT_EQ(out.size(), src.size());

  bool has_white = false, has_black = false;
  for (int y = 0; y < out.rows; ++y) {
    auto* row = out.ptr<cv::Vec3b>(y);
    for (int x = 0; x < out.cols; ++x) {
      const cv::Vec3b& v = row[x];
      ASSERT_TRUE(v[0] == 0 || v[0] == 255) << "二值化后不应有中间灰度";
      EXPECT_EQ(v[0], v[1]);
      EXPECT_EQ(v[1], v[2]);
      has_white |= (v[0] == 255);
      has_black |= (v[0] == 0);
    }
  }
  EXPECT_TRUE(has_white);
  EXPECT_TRUE(has_black);
}

TEST(OcrPreprocess, NoneModesArePassthrough) {
  cv::Mat src = MakeWhiteOnNoiseImage();
  PreprocessParams p;
  FillDefaultPreprocess(&p);
  cv::Mat out = ApplyPreprocess(src, p);
  EXPECT_EQ(out.size(), src.size());
  EXPECT_EQ(cv::norm(out, src), 0.0) << "默认参数下不应改变像素";
}

TEST(OcrPreprocess, WhiteTextExtractSeparatesOverlaySubtitleFromNoise) {
  // 这是本次修复的核心机制：白字在 V-S 响应图上必须明显亮于彩色背景。
  // 用亮黄做背景：它的灰度（≈226）与白字（≈250）几乎重叠，单看灰度分不
  // 开；但饱和度拉满，V-S 响应直接归零 —— 这正是监控画面里白字
  // 压在亮景物上的典型难分场景。
  cv::Mat img(80, 200, CV_8UC3, cv::Scalar(0, 255, 255));
  cv::rectangle(img, cv::Rect(10, 20, 80, 40), cv::Scalar(250, 250, 250),
                cv::FILLED);

  // 先确认两者在灰度图上确实接近（否则这个用例测不出什么）
  cv::Mat gray_src;
  cv::cvtColor(img, gray_src, cv::COLOR_BGR2GRAY);
  const double gray_text = cv::mean(gray_src(cv::Rect(10, 20, 80, 40)))[0];
  const double gray_bg = cv::mean(gray_src(cv::Rect(110, 20, 80, 40)))[0];
  EXPECT_LT(std::abs(gray_text - gray_bg), 60.0);

  PreprocessParams p;
  FillDefaultPreprocess(&p);
  p.enhance_mode = kEnhanceWhiteText;
  cv::Mat out = ApplyEnhance(img, p);
  ASSERT_EQ(out.size(), img.size());

  cv::Mat gray_out;
  cv::cvtColor(out, gray_out, cv::COLOR_BGR2GRAY);
  const double text_resp = cv::mean(gray_out(cv::Rect(10, 20, 80, 40)))[0];
  const double bg_resp = cv::mean(gray_out(cv::Rect(110, 20, 80, 40)))[0];
  EXPECT_GT(text_resp, bg_resp + 30.0)
      << "白字响应应明显高于同亮度的饱和彩色（text=" << text_resp
      << " bg=" << bg_resp << "）";
}

TEST(OcrPreprocess, DetectTextIsBrightFindsBothPolarities) {
  // 用真字（细笔画）而不是大色块：大色块内部高通响应为 0，测不出极性
  const cv::Point org(10, 40);
  cv::Mat light(60, 320, CV_8UC1, cv::Scalar(25));
  cv::putText(light, "SUBTITLE 08:55:21", org, cv::FONT_HERSHEY_SIMPLEX, 1.0,
              cv::Scalar(245), 2);
  EXPECT_TRUE(DetectTextIsBright(light)) << "暗底亮字应判为文字偏亮";

  cv::Mat dark(60, 320, CV_8UC1, cv::Scalar(245));
  cv::putText(dark, "SUBTITLE 08:55:21", org, cv::FONT_HERSHEY_SIMPLEX, 1.0,
              cv::Scalar(10), 2);
  EXPECT_FALSE(DetectTextIsBright(dark)) << "亮底暗字应判为文字偏暗";
}

TEST(OcrPreprocess, OpeningConnectsBrokenBlackStrokes) {
  // 事必求证：二值化后是白底黑字，开运算让黑色扩张，应把相邻短竖连起来
  auto MakeTwoStrokes = [] {
    cv::Mat src(40, 60, CV_8UC3, cv::Scalar(255, 255, 255));
    cv::rectangle(src, cv::Rect(20, 10, 3, 20), cv::Scalar(0, 0, 0), cv::FILLED);
    cv::rectangle(src, cv::Rect(26, 10, 3, 20), cv::Scalar(0, 0, 0), cv::FILLED);
    return src;
  };
  const cv::Rect gap(23, 15, 3, 10);  // 两竖之间的白色缝隙

  PreprocessParams p;
  FillDefaultPreprocess(&p);
  cv::Mat untouched = ApplyMorphology(MakeTwoStrokes(), p);
  EXPECT_GT(cv::mean(untouched(gap))[0], 250.0) << "核为 0 时应是空操作";

  p.morph_open_kernel = 2;  // 5x5
  cv::Mat opened = ApplyMorphology(MakeTwoStrokes(), p);
  const double darkness = 255.0 - cv::mean(opened(gap))[0];
  EXPECT_GT(darkness, 100.0) << "开运算应把断裂笔画连上";
}

TEST(OcrPreprocess, PrepareCropShrinksBlockSizeToCropHeight) {
  // 48px 高的文本条不应沿用整图的 31 邻域（会吃掉笔画）
  cv::Mat crop = cv::Mat(16, 200, CV_8UC3, cv::Scalar(30, 120, 30));
  cv::putText(crop, "AB", cv::Point(4, 13), cv::FONT_HERSHEY_SIMPLEX, 0.6,
              cv::Scalar(250, 250, 250), 1);
  PreprocessParams p;
  FillDefaultPreprocess(&p);
  p.binarize_mode = kBinarizeAdaptiveGaussInv;
  p.adaptive_block_size = 0;  // 0 = 按文本条高度自动收敛
  cv::Mat out = PrepareCropForRecognition(crop, p);
  EXPECT_EQ(out.size(), crop.size());
  EXPECT_EQ(out.channels(), 3);
}

// ===========================================================================
// ocr_preprocess.cpp —— 文本框几何与多分支合并
// ===========================================================================
namespace {

DetectedBox MakeDetected(int x, int y, int w, int h, int branch, double score) {
  DetectedBox d;
  d.points = MakeRect(x, y, w, h);
  d.branch_index = branch;
  d.score = score;
  return d;
}

MergeParams SimpleMerge() {
  MergeParams m;
  FillDefaultMerge(&m);
  return m;
}

}  // namespace

TEST(BoxGeometry, IoUAndContainment) {
  const auto a = MakeRect(0, 0, 100, 20);
  const auto same = MakeRect(0, 0, 100, 20);
  const auto far = MakeRect(500, 500, 100, 20);
  const auto inside = MakeRect(10, 2, 40, 16);
  const auto overlap_half = MakeRect(50, 0, 100, 20);

  EXPECT_NEAR(BoxIoU(a, same), 1.0, 1e-6);
  EXPECT_DOUBLE_EQ(BoxIoU(a, far), 0.0);
  EXPECT_NEAR(BoxIoU(a, overlap_half), 1.0 / 3.0, 1e-6);
  EXPECT_NEAR(BoxContainment(inside, a), 1.0, 1e-6);
  // inside 面积 640 / a 面积 2000 —— 反向包含度就是覆盖率
  EXPECT_NEAR(BoxContainment(a, inside), 0.32, 0.02);
  EXPECT_NEAR(BoxShortSide(a), 20.0, 1.0);
  EXPECT_NEAR(BoxLongSide(a), 100.0, 1.0);
  EXPECT_NEAR(BoxAngle(a), 0.0, 1.0) << "水平框倾角应为 0";
  // 退化输入不能崩
  EXPECT_DOUBLE_EQ(BoxIoU({}, a), 0.0);
  EXPECT_DOUBLE_EQ(BoxArea({cv::Point(1, 1)}), 0.0);
}

TEST(BoxGeometry, UnionBoxesCoversAllMembers) {
  const std::vector<std::vector<cv::Point>> parts = {
      MakeRect(100, 50, 60, 20), MakeRect(170, 50, 60, 20)};
  const auto u = UnionBoxes(parts);
  ASSERT_GE(u.size(), 3u);
  const cv::Rect r = BoxBoundingRect(u);
  EXPECT_LE(r.x, 100);
  EXPECT_GE(r.x + r.width, 230);
  EXPECT_NEAR(BoxShortSide(u), 20.0, 2.0);
}

TEST(MergeBoxes, OverlappingBoxesFromDifferentBranchesBecomeOneRegion) {
  const MergeParams m = SimpleMerge();
  std::vector<DetectedBox> boxes;
  boxes.push_back(MakeDetected(100, 50, 120, 22, /*branch=*/0, 0.9));
  boxes.push_back(MakeDetected(102, 51, 118, 21, /*branch=*/1, 0.8));
  boxes.push_back(MakeDetected(400, 300, 100, 20, /*branch=*/2, 0.7));

  const auto merged = MergeDetectedBoxes(boxes, m);
  ASSERT_EQ(merged.size(), 2u);

  // 第一个区域应由两路共同贡献，便于后续择优识别
  const auto& big = merged[0];
  EXPECT_EQ(big.branches.size(), 2u);
  EXPECT_NE(std::find(big.branches.begin(), big.branches.end(), 0),
            big.branches.end());
  EXPECT_NE(std::find(big.branches.begin(), big.branches.end(), 1),
            big.branches.end());
  EXPECT_DOUBLE_EQ(big.score, 0.9) << "合并后保留最高分支得分";
}

TEST(MergeBoxes, RespectsMaxRegionsCap) {
  MergeParams m = SimpleMerge();
  m.max_regions = 2;
  std::vector<DetectedBox> boxes;
  for (int i = 0; i < 6; ++i) {
    boxes.push_back(MakeDetected(i * 150, 10, 100, 20, i, 0.8));
  }
  const auto merged = MergeDetectedBoxes(boxes, m);
  EXPECT_EQ(merged.size(), 2u);
}

TEST(StitchSameLine, JoinsCollinearFragments) {
  MergeParams m = SimpleMerge();
  m.stitch_same_line = 1;
  std::vector<MergedBox> in;
  // 一行被切碎的监控字幕：「…路口球」+「G66-57」+「24」
  in.push_back(MakeMerged(MakeRect(827, 20, 149, 20), {0}, 0.9));
  in.push_back(MakeMerged(MakeRect(972, 22, 55, 18), {1}, 0.9));
  in.push_back(MakeMerged(MakeRect(1028, 23, 22, 17), {1}, 0.9));

  const auto out = StitchSameLine(in, m);
  ASSERT_EQ(out.size(), 1u);
  EXPECT_TRUE(out[0].stitched);
  EXPECT_EQ(out[0].member_count, 3);
  const cv::Rect r = BoxBoundingRect(out[0].points);
  EXPECT_LE(r.x, 827);
  EXPECT_GE(r.x + r.width, 1050 - 1);
}

TEST(StitchSameLine, KeepsSeparateSubtitlesApart) {
  // 回归：左侧独立字幕曾被逐跳吞进右侧行（gap<0 直接接受导致）
  MergeParams m = SimpleMerge();
  m.stitch_same_line = 1;
  std::vector<MergedBox> in;
  in.push_back(MakeMerged(MakeRect(20, 20, 200, 20), {0}, 0.9));    // 时间戳
  in.push_back(MakeMerged(MakeRect(790, 20, 260, 20), {1}, 0.9));   // 位置串
  const auto out = StitchSameLine(in, m);
  EXPECT_EQ(out.size(), 2u) << "两段独立字幕不应被拼成一行";
}

TEST(StitchSameLine, DoesNotRunWhenDisabled) {
  MergeParams m = SimpleMerge();
  m.stitch_same_line = 0;
  std::vector<MergedBox> in;
  in.push_back(MakeMerged(MakeRect(10, 10, 50, 20), {0}, 0.9));
  in.push_back(MakeMerged(MakeRect(62, 10, 50, 20), {0}, 0.9));
  const auto out = StitchSameLine(in, m);
  EXPECT_EQ(out.size(), 2u);
}

TEST(StitchSameLine, RefusesToMergeAcrossColumns) {
  MergeParams m = SimpleMerge();
  m.stitch_same_line = 1;
  m.line_max_width_ratio = 1.5;  // 极紧：不允许明显变宽
  std::vector<MergedBox> in;
  in.push_back(MakeMerged(MakeRect(10, 10, 40, 20), {0}, 0.9));
  in.push_back(MakeMerged(MakeRect(52, 10, 40, 20), {0}, 0.9));
  const auto out = StitchSameLine(in, m);
  EXPECT_EQ(out.size(), 2u);
}

TEST(PadRegions, ExpandsAndClampsToImage) {
  std::vector<MergedBox> regions;
  MergedBox mb;
  mb.points = MakeRect(0, 0, 100, 20);
  mb.ref_long_side = BoxLongSide(mb.points);
  mb.ref_short_side = BoxShortSide(mb.points);
  regions.push_back(mb);

  PadRegions(&regions, 0.5, 640, 480);
  const cv::Rect r = BoxBoundingRect(regions[0].points);
  EXPECT_GE(r.width, 110) << "左右各外扩 0.5 倍字高";
  EXPECT_LE(r.x, 0);
  EXPECT_GE(r.y, 0) << "越界点应被夹到图内";

  // 参考几何不应被外扩污染（择优打分要用它）
  EXPECT_NEAR(regions[0].ref_short_side, 20.0, 1.0);
}

TEST(ReadingOrder, SortsTopToBottomThenLeftToRight) {
  std::vector<MergedBox> boxes;
  boxes.push_back(MakeMerged(MakeRect(300, 100, 60, 20), {0}, 0.9));
  boxes.push_back(MakeMerged(MakeRect(100, 100, 60, 20), {0}, 0.9));
  boxes.push_back(MakeMerged(MakeRect(50, 10, 60, 20), {0}, 0.9));
  SortBoxesForReadingOrder(&boxes, 0);
  EXPECT_EQ(BoxBoundingRect(boxes[0].points).y, 10);
  EXPECT_EQ(BoxBoundingRect(boxes[1].points).x, 100);
  EXPECT_EQ(BoxBoundingRect(boxes[2].points).x, 300);
}

// ===========================================================================
// 多变体择优打分
// ===========================================================================

TEST(VariantScoring, GeometryFitPenalisesBothUnderAndOverReading) {
  // 区域 260x20 → 预期 13 个字高倍数。真值「一舍篮球场东北路口球G66-5724」
  // 的估算宽度最接近；读少了的和插字的都应被惩罚。
  const double long_side = 260, short_side = 20;
  const std::string good = "舍篮球场东北路口球G66-5724";
  const std::string too_short = "球场东北路口球";
  const std::string too_long = "舍篮球场东北路口球G66-5724444444444";

  const double s_good =
      VariantSelectionScore(4, good, 1.0f, long_side, short_side, true);
  const double s_short =
      VariantSelectionScore(4, too_short, 1.0f, long_side, short_side, true);
  const double s_long =
      VariantSelectionScore(4, too_long, 1.0f, long_side, short_side, true);

  EXPECT_GT(s_good, s_short) << "读少了必须输给读全的";
  EXPECT_GT(s_good, s_long) << "插字的乱码必须输给读全的";
}

TEST(VariantScoring, FullLineBeatsDroppedLeadingCharWithinFillTolerance) {
  // 实测回归（监控字幕行）：同一区域里「一舍篮球场东北路口球G66-5724」
  // 与「合篮球场东北路口球G66-5724」（行首「一」被并进「舍」）的估算宽度
  // 只差 ~7%。没有 ±10% 死区时，读全的那一行会因为 fill 略大于 1 被扣分，
  // 反而输给少读一个字的候选。
  const double long_side = 254, short_side = 19;  // 13.4 个字高
  const double full = VariantSelectionScore(
      4, "一舍篮球场东北路口球G66-5724", 1.0f, long_side, short_side, true);
  const double merged = VariantSelectionScore(
      4, "合篮球场东北路口球G66-5724", 1.0f, long_side, short_side, true);
  EXPECT_GT(full, merged) << "读全整行必须胜过少读一个字的近似读法";
}

TEST(VariantScoring, RawCropWinsTiesOverEnhancedCrop) {
  // 同一区域、同样契合度时，未二次增强的候选优先（实测增强会偶发丢字）
  const double long_side = 260, short_side = 20;
  const std::string text = "舍篮球场东北路口球G66-5724";
  const double raw =
      VariantSelectionScore(4, text, 1.0f, long_side, short_side, true);
  const double enhanced =
      VariantSelectionScore(4, text, 1.0f, long_side, short_side, false);
  EXPECT_GT(raw, enhanced);
}

TEST(VariantScoring, LengthModeIgnoresGeometry) {
  const double long_side = 260, short_side = 20;
  const double short_text =
      VariantSelectionScore(2, "AB", 1.0f, long_side, short_side, true);
  const double long_text =
      VariantSelectionScore(2, "ABCDEFGHIJKL", 0.2f, long_side, short_side,
                            false);
  EXPECT_GT(long_text, short_text);
}

TEST(TextWidthEstimate, DistinguishesCjkFromAscii) {
  EXPECT_NEAR(EstimateTextWidthInEm("中"), 1.0, 1e-6);
  EXPECT_NEAR(EstimateTextWidthInEm("A"), 0.60, 1e-6);
  EXPECT_NEAR(EstimateTextWidthInEm("-"), 0.35, 1e-6);
  EXPECT_NEAR(EstimateTextWidthInEm("中文AB"), 3.2, 1e-6);
  EXPECT_DOUBLE_EQ(EstimateTextWidthInEm(""), 0.0);
  // 非法字节不应导致死循环或崩溃
  std::string broken = "\xff\xfe";
  broken += "A";
  EXPECT_GT(EstimateTextWidthInEm(broken), 0.0);
}

// ===========================================================================
// FFI：选项接口
// ===========================================================================

TEST(FfiOptions, SizeMatchesStructAndInitFillsDefaults) {
  const int32_t size = pp_ocr_options_size();
  EXPECT_GT(size, 0);

  PpOcrOptions o{};
  pp_ocr_options_init(&o);
  EXPECT_EQ(o.struct_size, size);
  EXPECT_DOUBLE_EQ(o.det_thresh, 0.30);
  EXPECT_EQ(static_cast<int>(o.branches), PP_OCR_BRANCH_RAW);
  EXPECT_EQ(o.roi_w, 0);
}

TEST(FfiOptions, PresetFillsWiderConfiguration) {
  PpOcrOptions o{};
  ASSERT_EQ(pp_ocr_options_preset(PP_OCR_PRESET_SURVEILLANCE, &o), 1);
  EXPECT_NE(o.branches & PP_OCR_BRANCH_WHITE_TEXT, 0u);
  EXPECT_EQ(o.stitch_same_line, 1);
  EXPECT_EQ(o.rec_enhance_crops, 2);
  EXPECT_EQ(o.rec_variant_select, 4);
  // 预设名可用于日志
  EXPECT_STREQ(pp_ocr_preset_name(PP_OCR_PRESET_SURVEILLANCE), "surveillance");
}

TEST(FfiOptions, NullOutIsRejectedNotCrashing) {
  EXPECT_EQ(pp_ocr_options_preset(999, nullptr), 0);  // 未知预设 + null 双拦截
  pp_ocr_options_init(nullptr);  // 不应崩
  EXPECT_EQ(pp_ocr_options_sanitize(nullptr), -1);
}

TEST(FfiOptions, SanitizeClampsAndCountsFixes) {
  PpOcrOptions o{};
  pp_ocr_options_init(&o);
  o.det_thresh = -3.0;
  o.det_max_side_len = 0;
  o.branches = 0;
  const int32_t fixed = pp_ocr_options_sanitize(&o);
  EXPECT_GE(fixed, 3);
  EXPECT_GT(o.det_thresh, 0.0);
  EXPECT_GT(o.det_max_side_len, 0);
  EXPECT_NE(o.branches, 0u);
}

TEST_F(FfiApiTest, RecognizeFileExRejectsStaleStructSize) {
  PpOcrOptions o{};
  pp_ocr_options_init(&o);
  o.struct_size = 8;  // 模拟旧版 / 手写未填的结构体
  PpOcrResultArray r =
      pp_ocr_recognize_file_ex(engine_, "/nonexistent.png", &o);
  ASSERT_NE(r.error, nullptr);
  EXPECT_NE(std::strstr(r.error, "struct_size"), nullptr)
      << "布局不匹配必须显式报错，不能静默读乱码";
}

TEST_F(FfiApiTest, SetDefaultOptionsRejectsNullOptions) {
  EXPECT_EQ(pp_ocr_set_default_options(engine_, nullptr), 0);
  const char* err = pp_ocr_get_last_error(engine_);
  EXPECT_NE(err, nullptr);
  pp_ocr_clear_default_options(engine_);  // 不应崩
  pp_ocr_clear_default_options(nullptr);
}

TEST_F(FfiApiTest, RecognizeFileExWithNullOptionsMatchesLegacyEntry) {
  // options == nullptr 时，_ex 接口应与老接口一样报「引擎未初始化」
  PpOcrOptions o{};
  pp_ocr_options_init(&o);
  PpOcrResultArray legacy = pp_ocr_recognize_file(engine_, "/nope.png");
  PpOcrResultArray extended =
      pp_ocr_recognize_file_ex(engine_, "/nope.png", nullptr);
  ASSERT_NE(legacy.error, nullptr);
  ASSERT_NE(extended.error, nullptr);
  EXPECT_STREQ(legacy.error, extended.error);
}

// ===========================================================================
// 集成测试：使用真实模型文件 + 合成的图片，跑完整 OCR 管道。
//
// 模型目录定位优先级：
//   1. 环境变量 PP_OCR_MODEL_DIR
//   2. 相对当前工作目录的 ../model 与 ./model
//   3. 硬编码开发机默认位置
// 未找到时自动 SKIP，不拖挂 CI。
// ===========================================================================
namespace {

std::string FindModelDir() {
  std::vector<std::string> candidates;
  if (const char* env = std::getenv("PP_OCR_MODEL_DIR"); env && *env) {
    candidates.emplace_back(env);
  }
  candidates.emplace_back("../model");
  candidates.emplace_back("./model");
  candidates.emplace_back(
      "/home/ubuntu/project/paddle_ocr/model");
  for (const auto& c : candidates) {
    std::error_code ec;
    auto det = std::filesystem::path(c) / "det.onnx";
    auto rec = std::filesystem::path(c) / "inference.onnx";
    auto dict = std::filesystem::path(c) / "ppocr_v6_dict.txt";
    if (std::filesystem::exists(det, ec) &&
        std::filesystem::exists(rec, ec) &&
        std::filesystem::exists(dict, ec)) {
      return std::filesystem::absolute(c).string();
    }
  }
  return "";
}

// 在临时日得中写一张黑底白字的 PNG，返回绝对路径。
std::string WriteTestImage(const std::string& text) {
  static int counter = 0;
  auto tmp = std::filesystem::temp_directory_path();
  std::string path =
      (tmp / ("pp_ocr_test_" + std::to_string(getpid()) + "_" +
              std::to_string(counter++) + ".png"))
          .string();
  cv::Mat img = cv::Mat::zeros(120, 480, CV_8UC3);
  cv::putText(img, text, cv::Point(30, 75), cv::FONT_HERSHEY_SIMPLEX,
              /*fontScale=*/2.0, cv::Scalar(255, 255, 255), /*thickness=*/3);
  cv::imwrite(path, img);
  return path;
}

}  // namespace

class OcrPipelineIntegration : public ::testing::Test {
 protected:
  void SetUp() override {
    model_dir_ = FindModelDir();
    if (model_dir_.empty()) {
      GTEST_SKIP() << "model directory not found; set PP_OCR_MODEL_DIR";
    }
    det_ = model_dir_ + "/det.onnx";
    rec_ = model_dir_ + "/inference.onnx";
    dict_ = model_dir_ + "/ppocr_v6_dict.txt";
    engine_ = pp_ocr_create();
    ASSERT_NE(engine_, nullptr);
    int rc = pp_ocr_initialize(engine_, det_.c_str(), rec_.c_str(),
                                dict_.c_str());
    ASSERT_EQ(rc, 1)
        << "pp_ocr_initialize failed: "
        << (pp_ocr_get_last_error(engine_) ? pp_ocr_get_last_error(engine_) : "?");
  }
  void TearDown() override {
    if (engine_) pp_ocr_destroy(engine_);
    engine_ = nullptr;
  }

  std::string model_dir_, det_, rec_, dict_;
  PpOcrEngine* engine_ = nullptr;
};

TEST_F(OcrPipelineIntegration, RecognizeFileReturnsValidStruct) {
  std::string png = WriteTestImage("Hello");
  PpOcrResultArray r = pp_ocr_recognize_file(engine_, png.c_str());
  EXPECT_EQ(r.error, nullptr) << "recognize_file returned error: "
                              << (r.error ? r.error : "");
  EXPECT_GE(r.count, 0);
  std::error_code ec;
  std::filesystem::remove(png, ec);
}

TEST_F(OcrPipelineIntegration, RecognizeBytesReturnsValidStruct) {
  std::string png = WriteTestImage("World");
  std::ifstream f(png, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
  f.close();
  ASSERT_FALSE(bytes.empty());

  PpOcrResultArray r = pp_ocr_recognize_bytes(
      engine_, bytes.data(), static_cast<int>(bytes.size()));
  EXPECT_EQ(r.error, nullptr) << "recognize_bytes returned error: "
                              << (r.error ? r.error : "");
  EXPECT_GE(r.count, 0);
  std::error_code ec;
  std::filesystem::remove(png, ec);
}

TEST_F(OcrPipelineIntegration, RepeatedCallsDoNotCorruptBuffers) {
  // Regression guard: the FFI result array is stored on the engine and
  // reused across calls. Two sequential calls must both return valid
  // structs (see convertResults in pp_ocr_ffi.cpp which reserves to
  // prevent c_str() pointer invalidation).
  std::string a = WriteTestImage("AAA");
  std::string b = WriteTestImage("BBB");

  PpOcrResultArray r1 = pp_ocr_recognize_file(engine_, a.c_str());
  EXPECT_EQ(r1.error, nullptr);

  PpOcrResultArray r2 = pp_ocr_recognize_file(engine_, b.c_str());
  EXPECT_EQ(r2.error, nullptr);

  std::error_code ec;
  std::filesystem::remove(a, ec);
  std::filesystem::remove(b, ec);
}

// 本次修复的回归用例：「白字压彩色噪声背景」的监控式字幕，
// 单分支检测会碎裂丢字，多分支预处理 + 同行拼接应能读得更多。
TEST_F(OcrPipelineIntegration, SurveillancePresetReadsMoreThanPlainSinglePass) {
  cv::Mat noisy = MakeWhiteOnNoiseImage(720, 200);
  const std::string png = [&] {
    auto tmp = std::filesystem::temp_directory_path();
    std::string p = (tmp / ("pp_ocr_surv_" + std::to_string(getpid()) +
                            ".png"))
                        .string();
    cv::imwrite(p, noisy);
    return p;
  }();

  // 1) 标准单分支（历史行为）
  PpOcrResultArray plain = pp_ocr_recognize_file(engine_, png.c_str());
  ASSERT_EQ(plain.error, nullptr) << "标准路径不应报错";
  size_t plain_chars = 0;
  for (int i = 0; i < plain.count; ++i) {
    plain_chars += std::string(plain.items[i].text ? plain.items[i].text : "")
                       .size();
  }

  // 2) 监控预设（多分支 + 拼接 + 择优）
  PpOcrOptions opts{};
  ASSERT_EQ(pp_ocr_options_preset(PP_OCR_PRESET_SURVEILLANCE, &opts), 1);
  PpOcrResultArray rich =
      pp_ocr_recognize_file_ex(engine_, png.c_str(), &opts);
  ASSERT_EQ(rich.error, nullptr) << "多分支路径不应报错";
  size_t rich_chars = 0;
  for (int i = 0; i < rich.count; ++i) {
    rich_chars += std::string(rich.items[i].text ? rich.items[i].text : "")
                      .size();
  }

  EXPECT_GT(rich_chars, 0u) << "监控预设必须读出内容来";
  EXPECT_GE(rich_chars, plain_chars)
      << "多分支管道不应比单分支读得更少（plain=" << plain_chars
      << " rich=" << rich_chars << "）";

  std::error_code ec;
  std::filesystem::remove(png, ec);
}

// ROI 选项：只读上半部分时，不应报出下半部分的区域。
TEST_F(OcrPipelineIntegration, RoiLimitsRecognitionArea) {
  const std::string png = WriteTestImage("TOP");

  PpOcrOptions opts{};
  pp_ocr_options_init(&opts);
  opts.roi_x = 0;
  opts.roi_y = 0;
  opts.roi_w = 200;   // 图片宽 480，只取左半
  opts.roi_h = 120;

  PpOcrResultArray r = pp_ocr_recognize_file_ex(engine_, png.c_str(), &opts);
  ASSERT_EQ(r.error, nullptr);
  for (int i = 0; i < r.count; ++i) {
    for (int j = 0; j < 4; ++j) {
      EXPECT_LE(r.items[i].box[j * 2], 200.0)
          << "ROI 外的横坐标说明偏移没生效";
    }
  }

  std::error_code ec;
  std::filesystem::remove(png, ec);
}

// 引擎级默认选项：设置后不带 options 的老接口也应走多分支。
TEST_F(OcrPipelineIntegration, SetDefaultOptionsAffectsLegacyEntry) {
  PpOcrOptions opts{};
  ASSERT_EQ(pp_ocr_options_preset(PP_OCR_PRESET_SURVEILLANCE, &opts), 1);
  ASSERT_EQ(pp_ocr_set_default_options(engine_, &opts), 1);

  const std::string png = WriteTestImage("HELLO");
  PpOcrResultArray r = pp_ocr_recognize_file(engine_, png.c_str());
  EXPECT_EQ(r.error, nullptr);

  pp_ocr_clear_default_options(engine_);
  PpOcrResultArray r2 = pp_ocr_recognize_file(engine_, png.c_str());
  EXPECT_EQ(r2.error, nullptr);

  std::error_code ec;
  std::filesystem::remove(png, ec);
}

}  // namespace test
}  // namespace paddle_ocr
