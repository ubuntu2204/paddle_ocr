#ifndef PADDLE_OCR_DEBUG_UTILS_H_
#define PADDLE_OCR_DEBUG_UTILS_H_

#include <cstdio>
#include <cstdint>
#include <string>
#include <sstream>
#include <iomanip>

// Compile-time toggle for verbose debug logging.
// Set to 1 to enable detailed debug output to stderr.
#ifndef PADDLE_OCR_DEBUG
#define PADDLE_OCR_DEBUG 1
#endif

namespace paddle_ocr {

// ---------------------------------------------------------------------------
// Debug log macro - writes to stderr (visible in Flutter debug console).
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
// Convert a string's bytes to a hex dump string for debugging.
// e.g. "AB\xC3\x28" -> "41 42 C3 28"
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
// Validate a UTF-8 string and return a detailed error report.
// Returns empty string if the input is valid UTF-8.
// ---------------------------------------------------------------------------
inline std::string ValidateUtf8Detailed(const std::string& input) {
  size_t i = 0;
  size_t char_count = 0;
  while (i < input.size()) {
    unsigned char c = static_cast<unsigned char>(input[i]);
    int seq_len = 0;
    uint32_t codepoint = 0;

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

    if (i + seq_len > input.size()) {
      std::ostringstream oss;
      oss << "Invalid UTF-8 at byte offset " << i
          << ": " << seq_len << "-byte sequence truncated (only "
          << (input.size() - i) << " bytes remaining)";
      return oss.str();
    }

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

    // Check for overlong encodings
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

    // Check for surrogate halves (U+D800 - U+DFFF)
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
      std::ostringstream oss;
      oss << "Invalid UTF-8 at byte offset " << i
          << ": surrogate codepoint U+"
          << std::hex << std::uppercase << codepoint
          << " is not valid in UTF-8";
      return oss.str();
    }

    // Check for codepoints above U+10FFFF
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
  return "";  // valid
}

}  // namespace paddle_ocr

#endif  // PADDLE_OCR_DEBUG_UTILS_H_
