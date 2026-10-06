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

}  // namespace test
}  // namespace paddle_ocr
