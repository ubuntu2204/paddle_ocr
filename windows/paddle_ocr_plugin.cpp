#include "paddle_ocr_plugin.h"
#include "debug_utils.h"

#include <windows.h>
#include <commdlg.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar_windows.h>
#include <flutter/standard_method_codec.h>

#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace paddle_ocr {

// static
void PaddleOcrPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows *registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "paddle_ocr",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<PaddleOcrPlugin>();

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto &call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  registrar->AddPlugin(std::move(plugin));
}

PaddleOcrPlugin::PaddleOcrPlugin()
    : ocr_engine_(std::make_unique<OcrEngine>()) {}

PaddleOcrPlugin::~PaddleOcrPlugin() = default;

// Helper: Convert OCR results to EncodableValue (list of maps)
static flutter::EncodableValue ResultsToEncodable(
    const std::vector<OcrBoxResult>& results) {
  flutter::EncodableList list;
  OCR_LOG("ResultsToEncodable: converting %zu results", results.size());
  for (size_t idx = 0; idx < results.size(); ++idx) {
    const auto& r = results[idx];
    flutter::EncodableMap map;
    // Box: list of 4 points, each point is [x, y]
    flutter::EncodableList box_list;
    for (const auto& pt : r.box) {
      flutter::EncodableList point = {
          flutter::EncodableValue(pt.x),
          flutter::EncodableValue(pt.y)};
      box_list.push_back(flutter::EncodableValue(point));
    }
    map[flutter::EncodableValue("box")] = flutter::EncodableValue(box_list);

    // Validate UTF-8 before sending to Flutter
    std::string text = r.text;
    std::string utf8_err = ValidateUtf8Detailed(text);
    if (!utf8_err.empty()) {
      OCR_LOG("ResultsToEncodable: ERROR - result[%zu] text has invalid UTF-8: %s",
              idx, utf8_err.c_str());
      OCR_LOG("ResultsToEncodable: result[%zu] text hex: %s",
              idx, HexDump(text).c_str());
      // Last-resort sanitization: replace invalid bytes with '?'
      std::string safe;
      safe.reserve(text.size());
      size_t i = 0;
      while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        int seq_len = 0;
        if (c <= 0x7F) seq_len = 1;
        else if ((c & 0xE0) == 0xC0) seq_len = 2;
        else if ((c & 0xF0) == 0xE0) seq_len = 3;
        else if ((c & 0xF8) == 0xF0) seq_len = 4;
        else { safe += '?'; i++; continue; }
        if (i + seq_len > text.size()) { safe += '?'; break; }
        bool valid = true;
        for (int j = 1; j < seq_len; ++j) {
          if ((static_cast<unsigned char>(text[i + j]) & 0xC0) != 0x80) {
            valid = false; break;
          }
        }
        if (valid) {
          safe.append(text, i, seq_len);
        } else {
          safe += '?';
        }
        i += seq_len;
      }
      text = safe;
      OCR_LOG("ResultsToEncodable: result[%zu] sanitized text hex: %s",
              idx, HexDump(text).c_str());
    }

    map[flutter::EncodableValue("text")] =
        flutter::EncodableValue(text);
    map[flutter::EncodableValue("confidence")] =
        flutter::EncodableValue(static_cast<double>(r.confidence));
    list.push_back(flutter::EncodableValue(map));

    OCR_LOG("ResultsToEncodable: result[%zu] text_hex='%s' conf=%.4f",
            idx, HexDump(text).c_str(), r.confidence);
  }
  return flutter::EncodableValue(list);
}

void PaddleOcrPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {

  OCR_LOG("HandleMethodCall: method='%s'", method_call.method_name().c_str());

  const auto* args = std::get_if<flutter::EncodableMap>(method_call.arguments());

  if (method_call.method_name().compare("initialize") == 0) {
    if (!args) {
      OCR_LOG("initialize: no arguments provided");
      result->Error("INVALID_ARGS", "Arguments required for initialize");
      return;
    }

    std::string det_path, rec_path, dict_path;
    auto det_it = args->find(flutter::EncodableValue("detModelPath"));
    if (det_it != args->end()) {
      det_path = std::get<std::string>(det_it->second);
    }
    auto rec_it = args->find(flutter::EncodableValue("recModelPath"));
    if (rec_it != args->end()) {
      rec_path = std::get<std::string>(rec_it->second);
    }
    auto dict_it = args->find(flutter::EncodableValue("dictPath"));
    if (dict_it != args->end()) {
      dict_path = std::get<std::string>(dict_it->second);
    }

    OCR_LOG("initialize: detModelPath='%s' (hex: %s)",
            det_path.c_str(), HexDump(det_path).c_str());
    OCR_LOG("initialize: recModelPath='%s' (hex: %s)",
            rec_path.c_str(), HexDump(rec_path).c_str());
    OCR_LOG("initialize: dictPath='%s' (hex: %s)",
            dict_path.c_str(), HexDump(dict_path).c_str());

    if (det_path.empty() || rec_path.empty() || dict_path.empty()) {
      OCR_LOG("initialize: one or more paths are empty");
      result->Error("INVALID_ARGS",
                    "detModelPath, recModelPath, and dictPath are required");
      return;
    }

    bool ok = ocr_engine_->Initialize(det_path, rec_path, dict_path);
    if (ok) {
      OCR_LOG("initialize: returning success=true");
      result->Success(flutter::EncodableValue(true));
    } else {
      std::string err_msg = "Failed to initialize OCR engine";
      const std::string& detail = ocr_engine_->GetLastError();
      if (!detail.empty()) {
        err_msg += ": " + detail;
      }
      OCR_LOG("initialize: returning error: %s", err_msg.c_str());
      result->Error("INIT_FAILED", err_msg);
    }

  } else if (method_call.method_name().compare("recognizeImage") == 0) {
    if (!ocr_engine_->IsInitialized()) {
      OCR_LOG("recognizeImage: engine not initialized");
      result->Error("NOT_INITIALIZED",
                    "OCR engine not initialized. Call initialize() first.");
      return;
    }

    if (!args) {
      OCR_LOG("recognizeImage: no arguments provided");
      result->Error("INVALID_ARGS", "Arguments required");
      return;
    }

    std::string image_path;
    auto path_it = args->find(flutter::EncodableValue("imagePath"));
    if (path_it != args->end()) {
      image_path = std::get<std::string>(path_it->second);
    }

    OCR_LOG("recognizeImage: imagePath='%s' (hex: %s)",
            image_path.c_str(), HexDump(image_path).c_str());

    // Validate path is UTF-8
    std::string path_err = ValidateUtf8Detailed(image_path);
    if (!path_err.empty()) {
      OCR_LOG("recognizeImage: ERROR - imagePath has invalid UTF-8: %s",
              path_err.c_str());
    }

    if (image_path.empty()) {
      OCR_LOG("recognizeImage: imagePath is empty");
      result->Error("INVALID_ARGS", "imagePath is required");
      return;
    }

    auto results = ocr_engine_->RecognizeFromFile(image_path);
    OCR_LOG("recognizeImage: got %zu results, sending to Flutter",
            results.size());
    result->Success(ResultsToEncodable(results));

  } else if (method_call.method_name().compare("recognizeImageBytes") == 0) {
    if (!ocr_engine_->IsInitialized()) {
      OCR_LOG("recognizeImageBytes: engine not initialized");
      result->Error("NOT_INITIALIZED",
                    "OCR engine not initialized. Call initialize() first.");
      return;
    }

    if (!args) {
      OCR_LOG("recognizeImageBytes: no arguments provided");
      result->Error("INVALID_ARGS", "Arguments required");
      return;
    }

    auto bytes_it = args->find(flutter::EncodableValue("imageBytes"));
    if (bytes_it == args->end()) {
      OCR_LOG("recognizeImageBytes: imageBytes not found in args");
      result->Error("INVALID_ARGS", "imageBytes is required");
      return;
    }

    const auto& encodable_bytes =
        std::get<std::vector<uint8_t>>(bytes_it->second);
    OCR_LOG("recognizeImageBytes: received %zu bytes",
            encodable_bytes.size());

    auto results = ocr_engine_->RecognizeFromBytes(encodable_bytes);
    OCR_LOG("recognizeImageBytes: got %zu results, sending to Flutter",
            results.size());
    result->Success(ResultsToEncodable(results));

  } else if (method_call.method_name().compare("pickImage") == 0) {
    // Windows file picker dialog - use wide-char version (GetOpenFileNameW)
    // to correctly handle non-ASCII paths (Chinese, Japanese, etc.).
    // The ANSI version (GetOpenFileNameA) returns paths in the system's
    // ANSI codepage (e.g. GBK on Chinese Windows), which are NOT valid
    // UTF-8 and cause FormatException in Dart.
    wchar_t wfilename[MAX_PATH] = {0};
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter =
        L"Image Files\0*.png;*.jpg;*.jpeg;*.bmp;*.tiff;*.tif\0"
        L"All Files\0*.*\0";
    ofn.lpstrFile = wfilename;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    OCR_LOG("pickImage: opening file dialog (using GetOpenFileNameW)");

    if (GetOpenFileNameW(&ofn)) {
      // Convert wide string to UTF-8
      int len = WideCharToMultiByte(CP_UTF8, 0, wfilename, -1,
                                     nullptr, 0, nullptr, nullptr);
      std::string utf8_path(len - 1, 0);
      WideCharToMultiByte(CP_UTF8, 0, wfilename, -1,
                           &utf8_path[0], len, nullptr, nullptr);

      OCR_LOG("pickImage: selected path='%s'", utf8_path.c_str());
      OCR_LOG("pickImage: path hex: %s", HexDump(utf8_path).c_str());

      // Validate the UTF-8 path
      std::string path_err = ValidateUtf8Detailed(utf8_path);
      if (!path_err.empty()) {
        OCR_LOG("pickImage: ERROR - converted path still has invalid UTF-8: %s",
                path_err.c_str());
      }

      result->Success(flutter::EncodableValue(utf8_path));
    } else {
      DWORD err = CommDlgExtendedError();
      OCR_LOG("pickImage: dialog cancelled or error (CommDlgExtendedError=%lu)",
              err);
      result->Success(flutter::EncodableValue(""));
    }

  } else if (method_call.method_name().compare("getPlatformVersion") == 0) {
    OCR_LOG("getPlatformVersion: returning 'Windows 10+'");
    result->Success(flutter::EncodableValue("Windows 10+"));

  } else {
    OCR_LOG("HandleMethodCall: unknown method '%s'",
            method_call.method_name().c_str());
    result->NotImplemented();
  }
}

}  // namespace paddle_ocr
