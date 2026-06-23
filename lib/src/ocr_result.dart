import 'dart:ui';

/// Represents a single OCR detection result with bounding box, text, and confidence.
class OcrResult {
  /// The 4 corner points of the detected text region.
  /// Order: top-left, top-right, bottom-right, bottom-left.
  final List<Offset> box;

  /// The recognized text content.
  final String text;

  /// Recognition confidence score (0.0 ~ 1.0).
  final double confidence;

  const OcrResult({
    required this.box,
    required this.text,
    required this.confidence,
  });

  /// Parse from the map returned by the native method channel.
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
