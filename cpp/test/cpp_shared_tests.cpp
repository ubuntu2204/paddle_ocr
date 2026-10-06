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
#include <string>

#include "debug_utils.h"
#include "pp_ocr_ffi.h"

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

}  // namespace test
}  // namespace paddle_ocr
