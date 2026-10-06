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

## 0.3.0

* **新增 Linux 支持**
* 插件与 example 均可在 Linux 桌面（Ubuntu 22.04+ 等基于 glibc 的发行版）构建和运行
* 将 `ocr_engine.*`、`pp_ocr_ffi.*`、`debug_utils.h` 从 `windows/` 提升为 `cpp/` 共享目录，Windows/Linux 共用
* Windows 相关的宽字符路径 / ORT 会话代码均使用 `#ifdef _WIN32` 守卫；POSIX 下直接以 UTF-8 字节处理文件路径
* 新增 `linux/paddle_ocr_plugin.cc`：基于 `FlMethodChannel` 实现 `initialize`/`recognizeImage`/`recognizeImageBytes`/`pickImage`/`getPlatformVersion`；`pickImage` 使用 `GtkFileChooserDialog`
* 新增 `linux/CMakeLists.txt`：默认使用系统安装的 OpenCV（`apt install libopencv-dev`）与 ONNX Runtime；支持 `-DONNXRUNTIME_ROOT=<path>` 或 `linux/third_party/onnxruntime/` 内置方式
* `pubspec.yaml` 新增 `linux: pluginClass: PaddleOcrPlugin, ffiPlugin: true`
* Dart FFI 绑定支持 Linux，自动以 `DynamicLibrary.open('libpp_ocr_plugin.so')` 加载
* example 目录新增 `example/linux/` 脚手架（通过 `flutter create --platforms=linux` 生成）
* 新增 `tool/setup_linux_deps.sh`：当 GitHub Release 不可达时，从 PyPI 镜像自动拉取 `onnxruntime` wheel 并展开为 `linux/third_party/onnxruntime/{include,lib}`，自动建立 `libonnxruntime.so`、`libonnxruntime.so.1` SONAME 链接
* 修复 Linux 端实际接入时发现的 API 误用：`FlMethodMessenger` → `FlBinaryMessenger`；`fl_value_new_double` → `fl_value_new_float`；使用 `fl_value_set_string_take` 避免堆分配 key
* 插件 Linux 产物链接时自动向下游传递 `-rpath-link`，避免链接 runner 时报 `libonnxruntime.so.1 not found`
* `debug_utils.h` 里 `char_count` 增加 `(void)` 显式使用，修复 Linux 下 `-Werror=unused-but-set-variable`

## 0.3.1

* **测试覆盖扩展**
* 新增 Dart 单元测试：
  * `test/ocr_result_test.dart`——30 个用例，涵盖 `fromMap` 正常解析 / 默认值 / 异常输入 / 常量构造 / `toString`
  * `test/pp_ocr_ffi_test.dart`——验证 `FfiPaddleOcr` 在测试宿主上自动回退到 `MethodChannel`，以及 `dispose` 安全/幂等
  * `test/paddle_ocr_errors_test.dart`——验证 `PlatformException` 传播、`MissingPluginException`、参数 key 形式、返回值类型偏差
  * `test/pp_ocr_api_test.dart`——顶层 `PaddleOcr` 入口的懒加载、实例缓存、`dispose` 后重新读取
* 新增 C++ 共享单元测试 `cpp/test/cpp_shared_tests.cpp` + 独立可构建的 `cpp/test/CMakeLists.txt`（优先用系统 GoogleTest，否则 FetchContent），共 37 个用例，覆盖 `HexDump`、`ValidateUtf8Detailed` 与 `pp_ocr_ffi` C API
* 接入平台测试目标：`windows/CMakeLists.txt` 与 `linux/CMakeLists.txt` 的测试目标同时编译 `cpp_shared_tests.cpp`；`linux/test/paddle_ocr_plugin_test.cc` 验证导出符号可链接
* 拓展 `example/integration_test/plugin_integration_test.dart`：平台版本、真实模型 `initialize`、失败路径、`dispose` 幂等、垃圾图 `recognizeImage` 不崩等（模型目录自动探测，可用 `PP_OCR_MODEL_DIR` 覆盖）
* 新增 `tool/run_all_tests.sh`（可附带 `--with-build`）：一次性跑 `flutter analyze` + `flutter test` + `ctest`（+ 可选 `flutter build linux`）
* **修复测试暴露的两个真 bug**
  * `cpp/debug_utils.h::HexDump`：`std::hex` 未复位导致尾部字节总数被输出为十六进制（200 被写为 `C8`），补 `std::dec` 复位
  * `cpp/pp_ocr_ffi.cpp::errorResult`：`pp_ocr_recognize_file(nullptr, _)` 与 `pp_ocr_recognize_bytes(nullptr, _, _)` 会对空 `handle` 解引用而 SIGSEGV，改为在 `handle == nullptr` 时返回栈上临时 `PpOcrResultArray`

