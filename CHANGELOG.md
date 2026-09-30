## 0.1.0

* Initial release.
* Offline OCR using PaddleOCR PP-OCRv6 models with ONNX Runtime.
* Text detection (DB) and recognition (CRNN+CTC) pipeline.
* Unicode file path support on Windows (Chinese, Japanese, etc.).
* Detection box overlay with recognized text and confidence scores.
* Debug logging with UTF-8 validation utilities.

## 0.1.1

* 修复了一些依赖问题

## 0.1.2

* 修复了一些编译错误问题

## 0.1.3

* 内置 ONNX Runtime 1.20.1 和 OpenCV 4.9.0，无需下载或手动配置
* 内置文件位于 `windows/third_party/`
* Debug 构建使用 release DLL 运行，无需 debug DLL

## 0.2.0

* **重大更新：新增 FFI 后端**
* 默认使用 `dart:ffi` 直接调用原生 C++ 代码，延迟更低
* 自动回退到 MethodChannel（FFI DLL 不可用时）
* 新增 C API（`pp_ocr_ffi.h`）导出纯 C 函数供 FFI 调用
* 新增 `dispose()` 方法释放原生资源
* `pubspec.yaml` 添加 `ffiPlugin` 配置
* 更新 README 文档说明双后端架构

## 0.2.1

* 修复了一些因为中文路径的错误问题

## 0.2.2

* 修复与 MSVC 运行时混用时的 STL ABI 不兼容：跨 DLL 边界传递 `std::vector` 的 OpenCV 调用改为指针/C API/Mat 重载（`cvFindContours`、`minAreaRect(Mat)` 等），`cv::Mat` 布局不受 `_HAS_ITERATOR_DEBUGGING` 影响，Debug 构建不再崩溃
* 修复 llvm-mingw (libc++) 编译失败：`std::ifstream` 无 `std::wstring` 重载，改用 `std::filesystem::path` 构造宽路径（C++17 标准，MSVC/libc++ 双兼容，中文路径语义不变）
* `analysis_options.yaml` 排除 `build/` 与 `windows/` 目录的分析器检查

