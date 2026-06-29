# pp_ocr 示例应用

本示例演示了如何使用 `pp_ocr` 插件在 Flutter Windows 桌面应用中完成离线文字识别（OCR）。

示例应用基于 PaddleOCR 的 **PP-OCRv6** 模型，通过 ONNX Runtime 在本地进行推理，无需联网即可识别图片中的中英文文字。插件默认使用 `dart:ffi` 直接调用原生 C++ 引擎以获得更低的延迟，当 FFI 动态库不可用时会自动回退到 `MethodChannel` 后端。ONNX Runtime 与 OpenCV 已内置在插件中，无需额外下载或配置。

## 环境要求

在构建和运行本示例之前，请确保开发环境满足以下条件：

- **操作系统**：Windows 10 或更高版本
- **Flutter**：3.3.0 或更高版本
- **开发工具**：Visual Studio 2022，并安装「使用 C++ 的桌面开发」工作负载（包含 CMake 工具支持）
- **Dart SDK**：3.12.0 或更高版本

> **说明**：ONNX Runtime 1.20.1 与 OpenCV 4.9.0 已随插件一起打包在 `windows/third_party/` 目录下，构建时会自动链接，无需手动下载。

## 准备模型文件

本示例运行需要三个 PP-OCRv6 模型文件，请将它们放置在项目根目录下的 `model/` 文件夹中：

```
model/
├── det.onnx            # 文字检测模型
├── inference.onnx      # 文字识别模型
└── ppocr_v6_dict.txt   # 字符字典文件（约 18,708 个字符）
```

模型文件可以从 [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR) 官方仓库获取，并使用 [Paddle2ONNX](https://github.com/PaddlePaddle/Paddle2ONNX) 工具转换为 ONNX 格式。

示例应用启动后，默认会在界面顶部的模型配置栏中填写以下路径：

- `C:\project\paddle_ocr\model\det.onnx`
- `C:\project\paddle_ocr\model\inference.onnx`
- `C:\project\paddle_ocr\model\ppocr_v6_dict.txt`

如果你的模型文件位于其他位置，请在界面中手动修改对应的路径。

## 运行示例

在 `example/` 目录下依次执行以下命令：

```bash
# 1. 安装依赖
flutter pub get

# 2. 构建并运行 Windows 桌面应用
flutter run -d windows
```

首次构建时，CMake 会编译插件的原生 C++ 代码并链接内置的 ONNX Runtime 和 OpenCV，耗时可能较长，请耐心等待。构建完成后，应用窗口将自动打开。

## 使用说明

应用启动后，界面从上到下分为三个区域：模型配置栏、主内容区（左侧图片显示、右侧结果列表）和底部状态栏。

### 1. 初始化 OCR 引擎

在模型配置栏中确认或修改三个模型文件的路径后，点击 **Initialize** 按钮。引擎初始化成功后，状态栏会显示「OCR engine initialized successfully.」，按钮文字变为 **Re-init**。如果初始化失败，请检查模型文件路径是否正确、文件是否存在。

### 2. 选择图片并识别

初始化成功后，点击 **Pick & Recognize** 按钮，系统会弹出文件选择对话框。选择一张包含文字的图片（支持常见的 PNG、JPG 等格式），应用会自动完成以下操作：

- 加载并显示选中的图片
- 调用 OCR 引擎对图片进行文字检测与识别
- 在图片上叠加红色检测框，并在框上方标注识别出的文字

识别过程中状态栏会显示「Recognizing text... This may take a moment.」，完成后会显示识别到的文字区域数量。

### 3. 查看结果

右侧结果面板以列表形式展示所有识别到的文字区域，每个条目包含：

- 序号（从 1 开始）
- 识别出的文字内容
- 置信度（以百分比显示，例如 98.5%）

底部状态栏会实时显示当前操作状态、识别进度以及可能出现的错误信息。

## 项目结构说明

```
example/
├── lib/
│   └── main.dart              # 示例应用主程序，包含 UI 与 OCR 调用逻辑
├── windows/                   # Windows 平台原生构建文件（CMake 等）
├── integration_test/          # 集成测试
├── test/                      # 单元测试
├── analysis_options.yaml      # Dart 代码分析规则配置
├── pubspec.yaml               # 示例应用依赖配置
└── README.md                  # 本文档
```

其中 `lib/main.dart` 是示例应用的核心文件，主要包含以下部分：

- `OcrExampleApp`：应用根组件，配置 Material 3 主题
- `OcrDemoPage` / `_OcrDemoPageState`：主页面，负责模型配置、图片选择、OCR 调用与结果展示
- `OcrBoxPainter`：自定义绘制器，用于在图片上叠加文字检测框和识别结果标注
