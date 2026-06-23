import 'dart:io';
import 'dart:ui' as ui;
import 'dart:developer' as developer;

import 'package:flutter/material.dart';
import 'package:paddle_ocr/paddle_ocr.dart';

void main() {
  runApp(const OcrExampleApp());
}

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

class OcrDemoPage extends StatefulWidget {
  const OcrDemoPage({super.key});

  @override
  State<OcrDemoPage> createState() => _OcrDemoPageState();
}

class _OcrDemoPageState extends State<OcrDemoPage> {
  final PaddleOcr _ocr = PaddleOcr();

  // Model paths - defaults to the model/ folder in the project root
  // Update these paths if your models are located elsewhere
  final TextEditingController _detModelCtrl = TextEditingController(
    text: r'C:\project\paddle_ocr\model\det.onnx',
  );
  final TextEditingController _recModelCtrl = TextEditingController(
    text: r'C:\project\paddle_ocr\model\inference.onnx',
  );
  final TextEditingController _dictPathCtrl = TextEditingController(
    text: r'C:\project\paddle_ocr\model\ppocr_v6_dict.txt',
  );

  bool _initialized = false;
  bool _loading = false;
  String? _statusMessage;
  String? _imagePath;
  List<OcrResult> _results = [];
  double _imageWidth = 0;
  double _imageHeight = 0;

  @override
  void dispose() {
    _detModelCtrl.dispose();
    _recModelCtrl.dispose();
    _dictPathCtrl.dispose();
    super.dispose();
  }

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
      developer.log('Init error', error: e, stackTrace: st);
      setState(() {
        _statusMessage = 'Initialization error: $e\n'
            'Type: ${e.runtimeType}\n'
            'Stack: ${st.toString().split('\n').take(5).join(' | ')}';
      });
    } finally {
      setState(() => _loading = false);
    }
  }

  Future<void> _pickAndRecognize() async {
    if (!_initialized) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Please initialize OCR engine first')),
      );
      return;
    }

    setState(() {
      _loading = true;
      _statusMessage = 'Selecting image...';
      _results = [];
      _imagePath = null;
      _imageWidth = 0;
      _imageHeight = 0;
    });

    try {
      final path = await _ocr.pickImage();
      if (path == null || path.isEmpty) {
        setState(() {
          _statusMessage = 'No image selected.';
          _loading = false;
        });
        return;
      }

      // Load image dimensions for accurate box overlay
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

      final results = await _ocr.recognizeImage(path);

      setState(() {
        _results = results;
        _statusMessage =
            'Recognition complete. Found ${results.length} text region(s).';
        _loading = false;
      });
    } on FormatException catch (e) {
      developer.log('FormatException in pickAndRecognize', error: e);
      setState(() {
        _statusMessage = 'FormatException: ${e.message}\n'
            'Offset: ${e.offset}\n'
            'Type: ${e.runtimeType}\n'
            'This indicates invalid UTF-8 data from native code.\n'
            'Check native stderr logs for hex dumps.';
        _loading = false;
      });
    } catch (e, st) {
      developer.log('pickAndRecognize error', error: e, stackTrace: st);
      setState(() {
        _statusMessage = 'Error: $e\n'
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
          // Model config panel
          _buildConfigPanel(),
          const Divider(height: 1),
          // Main content
          Expanded(
            child: Row(
              children: [
                // Image display
                Expanded(flex: 3, child: _buildImagePanel()),
                const VerticalDivider(width: 1),
                // Results list
                Expanded(flex: 2, child: _buildResultsPanel()),
              ],
            ),
          ),
          // Status bar
          if (_statusMessage != null)
            Container(
              width: double.infinity,
              constraints: const BoxConstraints(maxHeight: 120),
              padding: const EdgeInsets.all(8),
              color: Colors.grey[200],
              child: SingleChildScrollView(
                child: SelectableText(
                  _statusMessage!,
                  style: const TextStyle(
                    fontSize: 12,
                    fontFamily: 'monospace',
                  ),
                ),
              ),
            ),
        ],
      ),
    );
  }

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
                // Image
                Center(
                  child: Image.file(File(_imagePath!), fit: BoxFit.contain),
                ),
                // Detection boxes overlay
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
                        leading: CircleAvatar(
                          radius: 14,
                          child: Text(
                            '${index + 1}',
                            style: const TextStyle(fontSize: 11),
                          ),
                        ),
                        title: Text(
                          r.text,
                          style: const TextStyle(fontSize: 13),
                        ),
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

/// Custom painter to draw OCR detection boxes over the image.
class OcrBoxPainter extends CustomPainter {
  final String imagePath;
  final List<OcrResult> results;
  final double imageWidth;
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

    final paint = Paint()
      ..color = Colors.red.withValues(alpha: 0.7)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2.0;

    // Compute scale to fit image into canvas while preserving aspect ratio.
    // The image is displayed with BoxFit.contain.
    final double scaleX = size.width / imageWidth;
    final double scaleY = size.height / imageHeight;
    final double scale = scaleX < scaleY ? scaleX : scaleY;

    // Actual displayed image size (letterboxed if aspect ratios differ)
    final double displayedW = imageWidth * scale;
    final double displayedH = imageHeight * scale;
    // Offset to center the image in the canvas
    final double offsetX = (size.width - displayedW) / 2;
    final double offsetY = (size.height - displayedH) / 2;

    final textPainter = TextPainter(textDirection: TextDirection.ltr);

    for (final result in results) {
      if (result.box.length < 4) continue;

      // Transform box coords from image space to canvas space
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

      // Draw text label
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
    return oldDelegate.results != results ||
        oldDelegate.imageWidth != imageWidth ||
        oldDelegate.imageHeight != imageHeight;
  }
}
