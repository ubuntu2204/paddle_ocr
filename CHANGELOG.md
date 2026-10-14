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

## 0.3.2

* **发布包瘦身（91MB → ~26MB）**：`model/`（det.onnx + inference.onnx + 字典，共约 77MB）不再随 pub 包 / git 仓库分发——插件运行不需要它们（模型路径由调用方传入），仅 example 自检与集成测试使用。模型获取方式见 README「模型文件」章节（GitHub Releases 附件或 Paddle2ONNX 自行转换）；本地开发布局不变（example 自动探测 `model/` 目录）。
* **移除 Windows 内嵌 OpenCV 二进制**（`opencv_world490` 的 dll/lib 共约 68MB）：所有 Windows 构建实际都走 `find_package(OpenCV)`（交叉编译注入的静态 OpenCV），内嵌 MSVC 二进制从未参与且已过时。头文件保留；如需在真 Windows + MSVC 环境构建，请设置 `-DOpenCV_DIR` 指向自己的 OpenCV（缺失时 CMake 给出明确指引，不再链接期神秘失败）。需要旧文件可从 git 历史找回。
* **修复 example 画框错位**：`OcrBoxPainter` 的缩放公式会"放大"小于面板的图片，而 `Image.file(fit: BoxFit.contain)` 在宽松约束下只缩小不放大——小图（如 291×340）的检测框整体按 `min(sx,sy)` 虚拟放大溢出图片显示区。修复：scale 加 `min(1.0, …)` 上限，与 RenderImage 语义严格一致。
* **example 界面显示版本号**：AppBar 显示 `v0.3.2`；底部状态栏常驻信息行显示 `app v0.3.2 | native: pp_ocr 0.3.2 | image: WxH`——Dart 包与原生库版本并列展示，可一眼识破部署错配（旧 DLL 混新 Dart 快照）。
* `lib/pp_ocr_ffi.dart` `getPlatformVersion()` 在 `initialize` 之前调用时永远走 MethodChannel（查不到原生库版本）——补上 `_ensureEngine()` 前置；`cpp/pp_ocr_ffi.cpp` 的 `kVersion` 从过时的 0.2.0 同步为 0.3.2。
* **修复 Linux example pickImage 选完图后闪退**
* 根因定位：内核日志 `segfault at 2 in gf_dri.so` —— Loongson GF 集显的 Mesa 驱动和 Flutter Impeller OpenGLESSDF 后端在 GTK dialog 开启/关闭时触发驱动段错误；同场景 Dart 层也会报 `Exception: No Impeller context is available`。不是插件自身的问题。
* [linux/paddle_ocr_plugin.cc](file:///home/ubuntu/project/paddle_ocr/linux/paddle_ocr_plugin.cc) 弹窗优先使用 **`GtkFileChooserNative`**：在有 `xdg-desktop-portal` 的环境下会交给独立子进程，完全避开本进程 GL；无 portal 时自动回退到 `GtkFileChooserDialog`，行为与之前一致。
* `OnPickerResponse` 统一处理 Native / Dialog 两种 signal 发送方；`PickerContext.widget` 改为 `gpointer`，避免 GObject/GtkWidget 强转假定；释放分叉：Native 走 `gtk_native_dialog_destroy`，Dialog 走 `gtk_widget_destroy`。
* 新增 **headless 自动化钩子**，方便 CI/无头复现，不需 xdotool：
  * `PP_OCR_PICKIMAGE_FAKE_PATH=<path>` — 插件不弹 dialog，直接 `g_idle_add` 异步回应固定路径（空串触发取消分支）
  * `PP_OCR_SELFTEST_IMAGE=<path>` — example 启动后自动跑 initialize → stat → readAsBytes → decodeImageFromList → recognizeImage → recognizeImageBytes 全链路，每步 `[SELFTEST]` 日志
  * `PP_OCR_SELFTEST_VIA_PICK=1` — self-test 先走一次 pickImage（搭配 fake path 即可无头验证完整时序）
* **新增 C++ 集成测试** `OcrPipelineIntegration`（直接调 `pp_ocr_initialize` + `pp_ocr_recognize_file/bytes`，使用仓库 `model/` 真实模型与 `cv::putText` 合成图）确认 OCR 管道在 ASCII / UTF-8 路径下完全健康。当前 ctest 共 40/40 全绿。
* [cpp/pp_ocr_ffi.cpp](file:///home/ubuntu/project/paddle_ocr/cpp/pp_ocr_ffi.cpp) `pp_ocr_recognize_file/bytes/initialize` 补上 ENTER/EXIT/中途日志，方便定位任何驱动层堆栈问题。
* **Linux 用户应急开关**（已写入 README）：
  * `LIBGL_ALWAYS_SOFTWARE=1 flutter run -d linux` — 一劳永逸避开 Mesa 驱动 bug（以性能换稳定性）
  * `GDK_BACKEND=x11` — 强制走 X11（部分 Wayland 合成器下也会缓解）



## 0.4.0

**修复「一行字只识别出一部分」**：白字压在复杂背景（监控叠加字幕、路牌、低对比扫描件）上时，
单路检测会把一行字碎裂成几段并丢头丢尾。现在整条管道可按需做多分支预处理召回。

* 新增 [cpp/ocr_options.h](file:///home/ubuntu/project/paddle_ocr/cpp/ocr_options.h)：
  检测 / 识别 / 预处理 / 多分支合并的全部可调参数与 6 个场景预设
  （`standard` / `document` / `lowContrast` / `surveillance` / `smallText` / `maxRecall`）。
  全部为定长 POD，可直接穿过 FFI。
* 新增 [cpp/ocr_preprocess.cpp](file:///home/ubuntu/project/paddle_ocr/cpp/ocr_preprocess.cpp)：
  二值化（Otsu / 自适应 / 自动极性）、CLAHE、锐化、去噪、**白色叠加字幕提取（V−S 响应）**、
  形态学、旋转框 IoU / 包含度、跨分支去重合并、同行碎框拼接、区域外扩、多变体择优打分。
* 识别管道升级为多分支：每路「预处理 + 检测参数」并行检测 → 跨分支合并去重 →
  同行碎框拼成整行 → 同一区域在多张图上各识别一次并择优。
  **二值化类分支只用于检测，其像素不会被喂给识别器**（否则大量乱码）。
* 择优默认策略 `variant_select = 4`：以「结果宽度 vs 区域长宽比」为标尺，同时惩罚
  读少了和插字读油了；同分时优先未二次增强的原像素候选。
  （PP-OCR 的 CTC 置信度极易饱和——漏 3 个字也报 1.0，单靠置信度选不出更全的结果。）
* C ABI 扩展（[cpp/pp_ocr_ffi.h](file:///home/ubuntu/project/paddle_ocr/cpp/pp_ocr_ffi.h)）：
  `PpOcrOptions` 结构体 + `pp_ocr_options_size/init/preset/name/sanitize`、
  `pp_ocr_set_default_options`、`pp_ocr_clear_default_options`、
  `pp_ocr_recognize_file_ex`、`pp_ocr_recognize_bytes_ex`。
  结构体带 `struct_size` 做版本协商，布局不匹配时**显式报错**而非静默读乱码。
* Dart API：新增 `OcrOptions` / `OcrPreset` / `OcrBranch` / `BinarizeMode` / `EnhanceMode` /
  `VariantSelection` / `ResultSortOrder`；`recognizeImage`、`recognizeImageBytes` 接受
  `options:`；新增 `recognizeImageWithPreset()`、`setDefaultOptions()`、`clearDefaultOptions()`。
  预设取值只由原生侧定义一处，`OcrOptions.preset(...)` 把它读成 Dart 对象后再微调。
* 新增 `roi_*` 选项：只识别矩形内的文字，结果坐标仍以原图为基准。
* 默认行为**完全不变**：不传 options 时仍走单分支标准路径，逐字与 0.3.x 一致。
* Linux / Windows 的 MethodChannel 回退路径同步支持 options
  （字段名单收敛在 `paddle_ocr::OptionsFromMapLookup` 一处，两个平台只做类型适配）。
* 测试：C++ 侧新增 38 个用例（选项合法性、预处理算子、框几何、跨分支合并、同行拼接、
  择优打分、FFI 选项接口），含两条回归——「同行拼接不得吞掉同一基线上的独立字幕」、
  「白字提取必须把白字从同亮度的饱和彩色里分出来」。ctest 78/78 全绿，Dart 105/105 全绿。
* 实测效果（1050×488 监控帧，字幕「一舍篮球场东北路口球G66-5724」压在树丛上）：
  0.3.x 默认参数 → `球场东北路口球0` + `G66-57`（碎成两段，丢头丢尾）；
  0.4.0 `surveillance` 预设 → `合篮球场东北路口球G66-5724`（完整一行）。
  仅剩首字「一」被模型与「舍」合并成「合」——该字是压在树叶上的 2px 单横笔画，
  属于 20px 字高下的模型识别极限，不是检测漏框。
* 新增 `tool/diag_build.sh`：本地调参 / 回归确认用的探针工具（不属于插件产物）。
