## 0.5.3（修复 runPipeline 段错误）

**修复 `PaddleOcr.runPipeline`（Dart 编排管线）必现段错误**：崩在 Dart 侧，
进程直接 SIGSEGV（无任何 Dart 异常），PC 落在 JIT code 区，极易误诊为
「Dart print / libc write」。

* 根因：`dart:ffi` 的 `Pointer<Struct>.ref` 语义是**视图而非拷贝**
  （SDK 文档原话 "A Dart view of the struct referenced by this pointer...
  backed by native memory at address"）。`pp_ocr_ffi_bindings.dart` 的
  `detectBytes` / `mergeBoxes` 写成 `return out.ref` 后由 `finally` 里
  `calloc.free(out)` —— 调用方拿到的结构体字段读的是已释放内存；随后任何
  malloc 复用该 32 字节 chunk（下一次变体检测的大量小分配），字段即变成
  垃圾值（`count` 变负数/巨数、`items` 变未映射地址），Dart 侧
  `detected.items[i]` 解引用野指针 → SIGSEGV。原生函数**按值返回**结构体
  （`recognizeFile` 等）由 VM 拷进新 compound，不受影响；同一文件里
  `preprocessBytes` / `recognizeBoxBytes` / `recognizeBoxesBytes` 因为在
  free 前已把字段读成 Dart 值 / 引擎持有的指针，同样不受影响。
* 修复：`detectBytes` / `mergeBoxes` 改为在 free 之前返回**字段快照**
  （Dart record：`items` / `count` / `imageWidth` / `imageHeight`），
  `items` 仍指向引擎持有的框数组（按 ABI 约定「下一次引擎调用前有效」，
  `runOcrPipeline` 的读取时机原本就满足）。调用点字段名不变。
* 新增回归护栏：
  * example selftest（`PP_OCR_SELFTEST_IMAGE`）新增 step 6 跑
    `runPipeline`（surveillance 同款 6 路变体），覆盖四个原子 FFI 接口
    的快照路径；
  * `example/integration_test` 新增 `runPipeline (Dart orchestration)
    does not crash`：自带最小灰度 PNG 生成器（zlib stored 块、无第三方
    依赖），白底黑横带保证 detect/merge 带真实数据执行。
* 顺手修正集成测试里 3 个过时期望（FFI-only 契约落地后一直红，与本次
  无关）：`getPlatformVersion` 接受 `pp_ocr x.y.z` 原生版本串；
  `initialize` 失败路径返回 `false` 而非抛 `PlatformException`；
  `recognizeImageBytes` 未初始化用例改传真 `Uint8List`（原先传
  `List<int> as dynamic` 触发同步类型错误，掩盖了被测路径）。
* 验证：主包 66 单测全过；example 集成测试 8/8 全过（含新回归用例，
  干净树上对照确认 3 个失败为预先存在）；example build linux ✓；
  监控帧 selftest 六步全过，`runPipeline` 稳定输出识别结果。

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

## 0.5.0

* **识别修复（「一舍篮球场东北路口球G66-5724」场景实测）**：0.4.0 的
  surveillance 预设读出 `合篮球场...`（丢行首「一」、「舍」误读「合」、残留
  重复碎片「724」）；0.5.0 读出完整的 `一舍篮球场东北路口球G66-5724`。
  三项根因修复：
  * **识别窗与区域框分离**：union 框会被贴边碎片撑歪（残框把高度撑大一倍，
    CRNN 缩放到 48px 后字带只占一半、开始读错字）。现在从成员框群体统计
    出字带（短边中位数 = 字高、y 中心中位数 = 行带），构造独立的「识别窗」；
    `MergedBox` 新增 `member_boxes` / `text_height`。
  * **行向外扩 `region_pad_long_ratio`**：DB 检测对紧邻行的孤立字符（单横
    「一」）没有响应，识别窗沿行方向向两侧各外扩 1.2 倍字高后，CRNN 能把
    紧邻的字读出来（surveillance / max_recall 预设默认开启）。
  * **合并判据与择优增强**：`BoxesAreDuplicate` 新增「垂直咬合 ≥ 50% 且水平
    重叠 > 70% 短宽」的水平带判据（消灭贴顶碎框「724」残留）；CTC 置信度
    饱和导致候选同分时，放大分支的候选胜出（1x 把「舍」读成「合」，
    2x/3x 正确）。
  * 新增 `kBranchUpscale3x` 分支（位 1<<9）：2x 对 20px 级小字仍会偶发
    繁简误读（篮→籃），3x 下实测完全正确。
* **MethodChannel 识别回退路线删除**：识别链路 100% 走 FFI，原生库不可用时
  抛 `StateError` 而不是静默降级（整图字节跨通道序列化的开销不可接受）。
  `OcrOptions.toMap/fromMap`、`OcrResult.fromMap` 随之删除。
  MethodChannel 仅保留 FFI 无法实现的功能（`pickImage` 原生文件对话框、
  `getPlatformVersion`），见 `PluginChannelPaddleOcr`。
* **预处理插件接口（管线编排）**：宿主 App 可注入自定义预处理并与引擎共享
  检测/合并/择优核心：
  * Dart：`OcrPreprocessor` 接口 + `NativeTransformPreprocessor`（参数驱动
    内置算子）/ `CustomTransformPreprocessor`（任意 Dart 图像处理）+
    `PaddleOcr.runPipeline()`；快捷构造 `upscalePreprocessor()`、
    `binarizePreprocessor()`、`whiteTextPreprocessor()`。
  * C：四个原子操作 `pp_ocr_preprocess_bytes` / `pp_ocr_detect_bytes` /
    `pp_ocr_merge_boxes` / `pp_ocr_recognize_box_bytes`（与完整管线共享同一套
    模型与几何后处理）；`PpOcrTransformParams` 平铺参数结构。
  * C++：`IImageTransform` / `PreprocessPipeline` 预处理管线接口（内置步骤
    可替换、宿主步骤可插入）、`OcrEngine::DetectOnImage / MergeRegions /
    RecognizeBox / ApplyImageTransform` 原子方法。
* 测试：C++ 78/78 全绿（新增识别窗构造、水平带去重、管线原子操作回归）；
  Dart 66/66 全绿（新增管线打分与插件接口契约测试）。
## 0.5.1

* **预设做减法**：`standard` / `document` / `lowContrast` / `smallText` 四个
  预设删除 —— 实测它们都无法读出「一舍篮球场东北路口球G66-5724」这类监控
  字幕（碎片化或整行丢失）。仅保留 `surveillance`（推荐默认）与 `maxRecall`。
  预设编号沿用发布时取值不重排（surveillance=3、maxRecall=5），原生
  `MakePresetOptions()` 对未知编号回退标准默认值。不传选项的引擎默认行为
  （单分支旧行为）不受影响。

## 0.5.2（识别速度优化，结果不变）

* **检测输入张量融合构建**：原实现是 `convertTo + split + 3 次通道算术 +
  merge + split + 3 次 memcpy` 约 10 趟全图扫描；改为一次遍历直接写 NCHW
  float 缓冲（`FillNchwTensor`），逐位复现原数值顺序与运算次序，模型输出
  不变。检测输入动辄 3000×1500，预处理阶段墙钟降一个量级。
* **相同输入只推理一次**：多分支管线里 raw 与 sensitive 的预处理产物逐字节
  相同（只有后处理阈值不同），原先跑两次整图推理。现在按张量内容分组
  （memcmp 判等），整组只跑一次模型，组内各分支用自己的 `DetectParams`
  做后处理。`TextDetector` 相应暴露 `BuildDetectInput` /
  `RunDetectInference` / `PostProcess` 分步接口。
* **检测阶段拆成「预处理 → 推理」两个 Phase**：OpenCV 与 ORT 各有线程池，
  混在同一个并行 lane 里互相抢核；分开后各自吃满。同时修正分支并行度
  （`kMaxBranchThreads` 8→2）——实测 DB 推理是内存带宽瓶颈，并发 Run 只
  会拖慢（24 核机器上 8 路 2089ms vs 2 路 1868ms）。
* **识别变体提前收尾（可证明安全）**：由打分公式
  `score = geometry + (raw_crop ? 2.0 : 0) + 0.001*scale` 与
  `EstimateTextWidthInEm` 的每字符 ≥0.35 字高权重，可推出 geometry 的
  理论上界；当前最优分一旦达到某类候选（增强/未增强）的上界，该类其余
  候选数学上不可能翻盘，直接跳过。典型监控帧的增强通道推理量减半。
  `expected_em ≤ 0.5` 的退化区域与 mode 1/2（长度无界）不启用早退。
* **FFI 解码缓存**：`pp_ocr_recognize_box_bytes` / `detect_bytes` 原先每次
  调用都对整张变体图重新 `cv::imdecode`（3x 变体图解码 ~78ms，而识别本身
  才几十毫秒）。改为按输入内容哈希缓存最近 8 份解码结果，Dart 管线
  「窗 × 变体」的重复解码全部命中。实测识别阶段 1475ms → 436ms。
* **FFI 预处理 PNG 压缩级别 3→1**：这些字节只被本引擎其它原子接口消费
  （解码缓存按内容哈希命中，与压缩率无关），3x 变体图编码 248ms → 167ms，
  体积几乎不变（5.4MB → 5.7MB）。
* **新增批量识别原子 API `pp_ocr_recognize_boxes_bytes`**：一次 FFI 穿越
  在同一张图上识别多个文本窗，原生侧线程池并行（`RecognizeBox` 的会话
  `Run` 可重入），图像只解码一次。Dart 侧 `runOcrPipeline` 改为按变体
  批量识别 + 逐窗择优，打分公式与顺序不变。实测 35 次识别 463ms → 311ms。
* **新增诊断工具**：`tool/diag/bench.cpp`（C++ 内部管线基准，输出结果签名
  供优化前后对照）、`tool/diag/bench_ffi.cpp`（复刻 Dart 编排的 FFI 链路
  基准，`PP_OCR_BENCH_BATCH=1` 走批量 API）、`cpp/paddle_ocr_timing.h`
  （分阶段墙钟计时，`PP_OCR_TIMING=1` 启用，默认零开销）。另修复检测输出
  统计循环（纯诊断用的全图 min/max/mean 扫描）只在调试构建里执行。
* 实测（24 核，3 张监控帧，best-of-3，结果签名逐位一致）：**总耗时
  8360ms → 6350ms（-24%）**。
* 遗留说明：`upscale3x` 分支的检测输入必须保持 3×base。实测降到 2×base
  （文字在检测输入里仍有 ~53px）会丢行首细笔画「一」的召回
  （「一舍篮球场…」退化成「合篮球场…」）。
