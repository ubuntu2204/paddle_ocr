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
* 内置文件位于 `windows/third_party/`：
  * ONNX Runtime: `onnxruntime.dll`、`onnxruntime.lib`、头文件
  * OpenCV: `opencv_world490.dll`、`opencv_world490.lib`、`opencv_world490d.lib`、头文件
* Debug 构建使用 release DLL (`opencv_world490.dll`) 运行，无需 debug DLL
* 如果遇到"找不到 `opencv_world490d.dll`"错误，请确保 CMake `bundled_libraries`
  正确配置，`opencv_world490.dll` 会自动复制到输出目录
