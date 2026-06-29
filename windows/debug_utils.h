#ifndef PADDLE_OCR_DEBUG_UTILS_H_
#define PADDLE_OCR_DEBUG_UTILS_H_

#include <cstdio>
#include <cstdint>
#include <string>
#include <sstream>
#include <iomanip>

// 编译期详细调试日志开关
// 设为 1 可启用输出到 stderr 的详细调试日志
#ifndef PADDLE_OCR_DEBUG
#define PADDLE_OCR_DEBUG 1
#endif

namespace paddle_ocr {

// ---------------------------------------------------------------------------
// 调试日志宏 - 输出到 stderr（在 Flutter 调试控制台中可见）
// ---------------------------------------------------------------------------
#if PADDLE_OCR_DEBUG
#define OCR_LOG(...) do { \
  fprintf(stderr, "[PaddleOcr] "); \
  fprintf(stderr, __VA_ARGS__); \
  fprintf(stderr, "\n"); \
  fflush(stderr); \
} while (0)
#else
#define OCR_LOG(...) ((void)0)
#endif

// ---------------------------------------------------------------------------
// 将字符串的字节转换为十六进制转储字符串，便于调试
// 例如 "AB\xC3\x28" -> "41 42 C3 28"
// ---------------------------------------------------------------------------
inline std::string HexDump(const std::string& s, size_t max_bytes = 128) {
  std::ostringstream oss;
  oss << std::hex << std::uppercase << std::setfill('0');
  size_t n = std::min(s.size(), max_bytes);
  for (size_t i = 0; i < n; ++i) {
    if (i > 0) oss << ' ';
    oss << std::setw(2) << static_cast<int>(static_cast<unsigned char>(s[i]));
  }
  if (s.size() > max_bytes) {
    oss << " ... (" << s.size() << " bytes total)";
  }
  return oss.str();
}

// ---------------------------------------------------------------------------
// 验证 UTF-8 字符串并返回详细的错误报告
// 输入为有效 UTF-8 时返回空字符串
// ---------------------------------------------------------------------------
inline std::string ValidateUtf8Detailed(const std::string& input) {
  size_t i = 0;
  size_t char_count = 0;
  while (i < input.size()) {
    unsigned char c = static_cast<unsigned char>(input[i]);
    int seq_len = 0;
    uint32_t codepoint = 0;

    // 根据首字节判断 UTF-8 序列长度并提取首字节携带的码点比特
    if (c <= 0x7F) {
      seq_len = 1;
      codepoint = c;
    } else if ((c & 0xE0) == 0xC0) {
      seq_len = 2;
      codepoint = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      seq_len = 3;
      codepoint = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      seq_len = 4;
      codepoint = c & 0x07;
    } else {
      std::ostringstream oss;
      oss << "Invalid UTF-8 at byte offset " << i
          << ": lead byte 0x" << std::hex << std::uppercase
          << static_cast<int>(c)
          << " is not a valid UTF-8 start byte";
      return oss.str();
    }

    // 检查序列是否被截断（剩余字节数不足）
    if (i + seq_len > input.size()) {
      std::ostringstream oss;
      oss << "Invalid UTF-8 at byte offset " << i
          << ": " << seq_len << "-byte sequence truncated (only "
          << (input.size() - i) << " bytes remaining)";
      return oss.str();
    }

    // 验证后续字节是否为合法的 UTF-8 续接字节（0x80-0xBF）
    for (int j = 1; j < seq_len; ++j) {
      unsigned char cb = static_cast<unsigned char>(input[i + j]);
      if ((cb & 0xC0) != 0x80) {
        std::ostringstream oss;
        oss << "Invalid UTF-8 at byte offset " << (i + j)
            << ": expected continuation byte (0x80-0xBF) but got 0x"
            << std::hex << std::uppercase << static_cast<int>(cb)
            << " in a " << seq_len << "-byte sequence starting at offset "
            << std::dec << i;
        return oss.str();
      }
      codepoint = (codepoint << 6) | (cb & 0x3F);
    }

    // 检查是否为过长编码（overlong encoding）
    if (seq_len == 2 && codepoint < 0x80) {
      std::ostringstream oss;
      oss << "Invalid UTF-8 at byte offset " << i
          << ": overlong 2-byte encoding for U+"
          << std::hex << std::uppercase << codepoint;
      return oss.str();
    }
    if (seq_len == 3 && codepoint < 0x800) {
      std::ostringstream oss;
      oss << "Invalid UTF-8 at byte offset " << i
          << ": overlong 3-byte encoding for U+"
          << std::hex << std::uppercase << codepoint;
      return oss.str();
    }
    if (seq_len == 4 && codepoint < 0x10000) {
      std::ostringstream oss;
      oss << "Invalid UTF-8 at byte offset " << i
          << ": overlong 4-byte encoding for U+"
          << std::hex << std::uppercase << codepoint;
      return oss.str();
    }

    // 检查是否为代理项半字符（U+D800 - U+DFFF，UTF-8 中非法）
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
      std::ostringstream oss;
      oss << "Invalid UTF-8 at byte offset " << i
          << ": surrogate codepoint U+"
          << std::hex << std::uppercase << codepoint
          << " is not valid in UTF-8";
      return oss.str();
    }

    // 检查码点是否超出 Unicode 上限 U+10FFFF
    if (codepoint > 0x10FFFF) {
      std::ostringstream oss;
      oss << "Invalid UTF-8 at byte offset " << i
          << ": codepoint U+"
          << std::hex << std::uppercase << codepoint
          << " exceeds U+10FFFF";
      return oss.str();
    }

    i += seq_len;
    char_count++;
  }
  return "";  // 验证通过，返回空字符串
}

}  // namespace paddle_ocr

#endif  // PADDLE_OCR_DEBUG_UTILS_H_
