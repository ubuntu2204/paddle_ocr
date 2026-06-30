import 'dart:io';
import 'dart:ui' as ui;
import 'dart:developer' as developer;

import 'package:flutter/material.dart';
import 'package:pp_ocr/pp_ocr.dart';

/// 应用程序入口函数
void main() {
  runApp(const OcrExampleApp());
}

/// OCR 示例应用根组件
class OcrExampleApp extends StatelessWidget {
  const OcrExampleApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'PP-OCRv6 Demo',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.blue),
        useMaterial3: true,
      ),
      home: const OcrDemoPage(),
    );
  }
}

/// OCR 演示页面（有状态组件）
class OcrDemoPage extends StatefulWidget {
  const OcrDemoPage({super.key});

  @override
  State<OcrDemoPage> createState() => _OcrDemoPageState();
}

/// OCR 演示页面的状态管理类
class _OcrDemoPageState extends State<OcrDemoPage> {
  // PaddleOcr 引擎实例
  final PaddleOcr _ocr = PaddleOcr();

  // 模型路径配置 - 自动解析项目根目录下 model/ 文件夹的绝对路径，
  // 避免硬编码绝对路径，使项目可在任意路径（含中文）下运行。
  // 如模型位于其他位置，可在界面输入框中修改。
  final TextEditingController _detModelCtrl = TextEditingController(
    text: _modelPath('det.onnx'),
  );
  final TextEditingController _recModelCtrl = TextEditingController(
    text: _modelPath('inference.onnx'),
  );
  final TextEditingController _dictPathCtrl = TextEditingController(
    text: _modelPath('ppocr_v6_dict.txt'),
  );

  /// 解析模型文件的绝对路径。
  ///
  /// model 文件夹位于项目根目录（example 的上一级）。
  /// `flutter run` 时工作目录为 example/，因此通过 `../model` 解析。
  static String _modelPath(String name) {
    final modelDir = Directory('${Directory.current.path}/../model');
    return File('${modelDir.absolute.path}/$name').absolute.path;
  }

  // OCR 引擎是否已初始化
  bool _initialized = false;
  // 是否正在执行加载/识别操作
  bool _loading = false;
  // 状态信息（显示在底部状态栏）
  String? _statusMessage;
  // 当前选择的图片路径
  String? _imagePath;
  // OCR 识别结果列表
  List<OcrResult> _results = [];
  // 原始图片宽度（用于精确绘制检测框）
  double _imageWidth = 0;
  // 原始图片高度（用于精确绘制检测框）
  double _imageHeight = 0;

  @override
  void dispose() {
    // 释放所有文本控制器
    _detModelCtrl.dispose();
    _recModelCtrl.dispose();
    _dictPathCtrl.dispose();
    super.dispose();
  }

  /// 初始化 OCR 引擎
  ///
  /// 使用用户配置的模型路径调用 [PaddleOcr.initialize]，
  /// 成功后会将 [_initialized] 置为 true。
  Future<void> _initOcr() async {
    setState(() {
      _loading = true;
      _statusMessage = 'Initializing OCR engine...';
    });
    try {
      final ok = await _ocr.initialize(
        detModelPath: _detModelCtrl.text.trim(),
        recModelPath: _recModelCtrl.text.trim(),
        dictPath: _dictPathCtrl.text.trim(),
      );
      setState(() {
        _initialized = ok;
        _statusMessage = ok
            ? 'OCR engine initialized successfully.'
            : 'Failed to initialize OCR engine. Check model paths.';
      });
    } catch (e, st) {
      // 记录初始化异常到开发者日志
      developer.log('Init error', error: e, stackTrace: st);
      setState(() {
        _statusMessage =
            'Initialization error: $e\n'
            'Type: ${e.runtimeType}\n'
            'Stack: ${st.toString().split('\n').take(5).join(' | ')}';
      });
    } finally {
      setState(() => _loading = false);
    }
  }

  /// 选择图片并执行 OCR 识别
  ///
  /// 流程：
  /// 1. 检查 OCR 引擎是否已初始化
  /// 2. 调用 [PaddleOcr.pickImage] 让用户选择图片
  /// 3. 解码图片获取原始尺寸（用于检测框叠加）
  /// 4. 调用 [PaddleOcr.recognizeImage] 执行识别
  Future<void> _pickAndRecognize() async {
    if (!_initialized) {
      // 未初始化时提示用户
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Please initialize OCR engine first')),
      );
      return;
    }

    setState(() {
      _loading = true;
      _statusMessage = 'Selecting image...';
      // 重置上次识别的状态
      _results = [];
      _imagePath = null;
      _imageWidth = 0;
      _imageHeight = 0;
    });

    try {
      final path = await _ocr.pickImage();
      if (path == null || path.isEmpty) {
        // 用户未选择图片
        setState(() {
          _statusMessage = 'No image selected.';
          _loading = false;
        });
        return;
      }

      // 加载图片尺寸，用于精确叠加检测框
      final imageFile = File(path);
      final decodedImage = await decodeImageFromList(
        await imageFile.readAsBytes(),
      );

      setState(() {
        _imagePath = path;
        _imageWidth = decodedImage.width.toDouble();
        _imageHeight = decodedImage.height.toDouble();
        _statusMessage = 'Recognizing text... This may take a moment.';
      });

      // 执行 OCR 识别
      final results = await _ocr.recognizeImage(path);

      setState(() {
        _results = results;
        _statusMessage =
            'Recognition complete. Found ${results.length} text region(s).';
        _loading = false;
      });
    } on FormatException catch (e) {
      // 处理 UTF-8 解码异常（通常来自原生层返回的无效数据）
      developer.log('FormatException in pickAndRecognize', error: e);
      setState(() {
        _statusMessage =
            'FormatException: ${e.message}\n'
            'Offset: ${e.offset}\n'
            'Type: ${e.runtimeType}\n'
            'This indicates invalid UTF-8 data from native code.\n'
            'Check native stderr logs for hex dumps.';
        _loading = false;
      });
    } catch (e, st) {
      // 处理其他未预期的异常
      developer.log('pickAndRecognize error', error: e, stackTrace: st);
      setState(() {
        _statusMessage =
            'Error: $e\n'
            'Type: ${e.runtimeType}\n'
            'Stack: ${st.toString().split('\n').take(5).join(' | ')}';
        _loading = false;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('PP-OCRv6 Offline OCR Demo'),
        backgroundColor: Theme.of(context).colorScheme.inversePrimary,
      ),
      body: Column(
        children: [
          // 模型配置面板
          _buildConfigPanel(),
          const Divider(height: 1),
          // 主内容区域
          Expanded(
            child: Row(
              children: [
                // 图片显示区域
                Expanded(flex: 3, child: _buildImagePanel()),
                const VerticalDivider(width: 1),
                // 识别结果列表
                Expanded(flex: 2, child: _buildResultsPanel()),
              ],
            ),
          ),
          // 底部状态栏（仅在有状态消息时显示）
          if (_statusMessage != null)
            Container(
              width: double.infinity,
              constraints: const BoxConstraints(maxHeight: 120),
              padding: const EdgeInsets.all(8),
              color: Colors.grey[200],
              child: SingleChildScrollView(
                child: SelectableText(
                  _statusMessage!,
                  style: const TextStyle(fontSize: 12, fontFamily: 'monospace'),
                ),
              ),
            ),
        ],
      ),
    );
  }

  /// 构建模型配置面板
  ///
  /// 包含检测模型路径、识别模型路径、字典路径的输入框，
  /// 以及"初始化"和"选择图片并识别"两个按钮。
  Widget _buildConfigPanel() {
    return Padding(
      padding: const EdgeInsets.all(12),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const Text(
            'Model Configuration',
            style: TextStyle(fontWeight: FontWeight.bold, fontSize: 14),
          ),
          const SizedBox(height: 8),
          Row(
            children: [
              // 检测模型路径输入框
              Expanded(
                child: TextField(
                  controller: _detModelCtrl,
                  decoration: const InputDecoration(
                    labelText: 'Det Model Path (.onnx)',
                    isDense: true,
                    border: OutlineInputBorder(),
                  ),
                  style: const TextStyle(fontSize: 12),
                ),
              ),
              const SizedBox(width: 8),
              // 识别模型路径输入框
              Expanded(
                child: TextField(
                  controller: _recModelCtrl,
                  decoration: const InputDecoration(
                    labelText: 'Rec Model Path (.onnx)',
                    isDense: true,
                    border: OutlineInputBorder(),
                  ),
                  style: const TextStyle(fontSize: 12),
                ),
              ),
              const SizedBox(width: 8),
              // 字典路径输入框
              Expanded(
                child: TextField(
                  controller: _dictPathCtrl,
                  decoration: const InputDecoration(
                    labelText: 'Dict Path (.txt)',
                    isDense: true,
                    border: OutlineInputBorder(),
                  ),
                  style: const TextStyle(fontSize: 12),
                ),
              ),
              const SizedBox(width: 8),
              // 初始化按钮（加载中显示进度指示器）
              ElevatedButton.icon(
                onPressed: _loading ? null : _initOcr,
                icon: _loading && !_initialized
                    ? const SizedBox(
                        width: 16,
                        height: 16,
                        child: CircularProgressIndicator(strokeWidth: 2),
                      )
                    : const Icon(Icons.settings),
                label: Text(_initialized ? 'Re-init' : 'Initialize'),
              ),
              const SizedBox(width: 8),
              // 选择图片并识别按钮（仅在已初始化且非加载状态时可用）
              ElevatedButton.icon(
                onPressed: _loading || !_initialized ? null : _pickAndRecognize,
                icon: const Icon(Icons.image_search),
                label: const Text('Pick & Recognize'),
              ),
            ],
          ),
        ],
      ),
    );
  }

  /// 构建图片显示面板
  ///
  /// 未选择图片时显示占位提示；
  /// 选择图片后显示图片，并通过 [OcrBoxPainter] 叠加绘制检测框。
  Widget _buildImagePanel() {
    if (_imagePath == null) {
      return const Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(Icons.image_outlined, size: 80, color: Colors.grey),
            SizedBox(height: 16),
            Text(
              'Click "Pick & Recognize" to select an image',
              style: TextStyle(color: Colors.grey),
            ),
          ],
        ),
      );
    }

    return Column(
      children: [
        Expanded(
          child: Padding(
            padding: const EdgeInsets.all(8),
            child: Stack(
              children: [
                // 显示选中的图片
                Center(
                  child: Image.file(File(_imagePath!), fit: BoxFit.contain),
                ),
                // 检测框叠加层
                if (_results.isNotEmpty)
                  Positioned.fill(
                    child: CustomPaint(
                      painter: OcrBoxPainter(
                        imagePath: _imagePath!,
                        results: _results,
                        imageWidth: _imageWidth,
                        imageHeight: _imageHeight,
                      ),
                    ),
                  ),
              ],
            ),
          ),
        ),
      ],
    );
  }

  /// 构建识别结果列表面板
  ///
  /// 显示所有识别到的文本区域，每项包含序号、文本内容和置信度。
  Widget _buildResultsPanel() {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.all(12),
          child: Text(
            'Recognition Results (${_results.length})',
            style: const TextStyle(fontWeight: FontWeight.bold, fontSize: 14),
          ),
        ),
        Expanded(
          child: _results.isEmpty
              ? const Center(
                  child: Text(
                    'No results yet',
                    style: TextStyle(color: Colors.grey),
                  ),
                )
              : ListView.builder(
                  itemCount: _results.length,
                  padding: const EdgeInsets.symmetric(horizontal: 8),
                  itemBuilder: (context, index) {
                    final r = _results[index];
                    return Card(
                      margin: const EdgeInsets.symmetric(vertical: 2),
                      child: ListTile(
                        dense: true,
                        // 序号头像
                        leading: CircleAvatar(
                          radius: 14,
                          child: Text(
                            '${index + 1}',
                            style: const TextStyle(fontSize: 11),
                          ),
                        ),
                        // 识别文本
                        title: Text(
                          r.text,
                          style: const TextStyle(fontSize: 13),
                        ),
                        // 置信度（百分比形式）
                        subtitle: Text(
                          'Confidence: ${(r.confidence * 100).toStringAsFixed(1)}%',
                          style: const TextStyle(fontSize: 11),
                        ),
                      ),
                    );
                  },
                ),
        ),
      ],
    );
  }
}

/// 自定义画笔，用于在图片上绘制 OCR 检测框
///
/// 根据 [imageWidth] 和 [imageHeight] 将检测框坐标从图片空间
/// 转换到画布空间，确保检测框与显示的图片精确对齐。
class OcrBoxPainter extends CustomPainter {
  /// 图片路径
  final String imagePath;
  /// OCR 识别结果列表
  final List<OcrResult> results;
  /// 原始图片宽度
  final double imageWidth;
  /// 原始图片高度
  final double imageHeight;

  OcrBoxPainter({
    required this.imagePath,
    required this.results,
    required this.imageWidth,
    required this.imageHeight,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (imageWidth <= 0 || imageHeight <= 0) return;

    // 检测框画笔配置：红色半透明描边
    final paint = Paint()
      ..color = Colors.red.withValues(alpha: 0.7)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2.0;

    // 计算将图片等比缩放到画布的缩放比例
    // 图片使用 BoxFit.contain 方式显示
    final double scaleX = size.width / imageWidth;
    final double scaleY = size.height / imageHeight;
    final double scale = scaleX < scaleY ? scaleX : scaleY;

    // 实际显示的图片尺寸（宽高比不同时会出现留白）
    final double displayedW = imageWidth * scale;
    final double displayedH = imageHeight * scale;
    // 图片在画布中的居中偏移量
    final double offsetX = (size.width - displayedW) / 2;
    final double offsetY = (size.height - displayedH) / 2;

    // 文本标签绘制器
    final textPainter = TextPainter(textDirection: TextDirection.ltr);

    for (final result in results) {
      // 检测框至少需要 4 个点
      if (result.box.length < 4) continue;

      // 将检测框坐标从图片空间转换到画布空间
      final path = ui.Path();
      path.moveTo(
        offsetX + result.box[0].dx * scale,
        offsetY + result.box[0].dy * scale,
      );
      for (int i = 1; i < result.box.length; i++) {
        path.lineTo(
          offsetX + result.box[i].dx * scale,
          offsetY + result.box[i].dy * scale,
        );
      }
      path.close();
      canvas.drawPath(path, paint);

      // 绘制文本标签（位于检测框左上角上方）
      textPainter.text = TextSpan(
        text: result.text,
        style: const TextStyle(
          color: Colors.red,
          fontSize: 10,
          backgroundColor: Colors.white70,
        ),
      );
      textPainter.layout(maxWidth: size.width);
      textPainter.paint(
        canvas,
        Offset(
          offsetX + result.box[0].dx * scale,
          offsetY + result.box[0].dy * scale - 14,
        ),
      );
    }
  }

  @override
  bool shouldRepaint(covariant OcrBoxPainter oldDelegate) {
    // 当结果列表、图片宽高发生变化时需要重绘
    return oldDelegate.results != results ||
        oldDelegate.imageWidth != imageWidth ||
        oldDelegate.imageHeight != imageHeight;
  }
}
