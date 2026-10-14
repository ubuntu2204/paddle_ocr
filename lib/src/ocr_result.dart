import 'dart:ui';

/// 表示单个 OCR 检测结果，包含边界框、文本内容和置信度。
class OcrResult {
  /// 检测到的文本区域的 4 个角点。
  /// 顺序为：左上、右上、右下、左下。
  final List<Offset> box;

  /// 识别出的文本内容。
  final String text;

  /// 识别置信度分数（0.0 ~ 1.0）。
  final double confidence;

  const OcrResult({
    required this.box,
    required this.text,
    required this.confidence,
  });

  @override
  String toString() =>
      'OcrResult(text: "$text", confidence: ${confidence.toStringAsFixed(3)}, '
      'box: $box)';
}
