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

  /// 从原生 MethodChannel 返回的 map 解析构造 [OcrResult]。
  factory OcrResult.fromMap(Map<dynamic, dynamic> map) {
    final rawBox = map['box'] as List<dynamic>;
    final box = rawBox.map<Offset>((point) {
      final p = point as List<dynamic>;
      return Offset(
        (p[0] as num).toDouble(),
        (p[1] as num).toDouble(),
      );
    }).toList();

    return OcrResult(
      box: box,
      text: map['text'] as String? ?? '',
      confidence: (map['confidence'] as num?)?.toDouble() ?? 0.0,
    );
  }

  @override
  String toString() =>
      'OcrResult(text: "$text", confidence: ${confidence.toStringAsFixed(3)}, '
      'box: $box)';
}
