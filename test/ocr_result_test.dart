import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:pp_ocr/src/ocr_result.dart';

/// [OcrResult] 的单元测试。
///
/// 覆盖：
/// - `fromMap` 对 box / text / confidence 的正常解析与默认值处理
/// - 混合数值类型（int / double）的坐标与置信度
/// - 缺字段 / 空 box / 非 4 点 box / 非数字坐标等边界与异常输入
/// - 相等性、toString、常量构造等通用行为
void main() {
  group('OcrResult.fromMap - normal parsing', () {
    test('parses 4-point box with double coordinates', () {
      final r = OcrResult.fromMap({
        'box': [
          [1.5, 2.5],
          [3.5, 4.5],
          [5.5, 6.5],
          [7.5, 8.5],
        ],
        'text': 'Hello',
        'confidence': 0.987,
      });
      expect(r.text, 'Hello');
      expect(r.confidence, 0.987);
      expect(r.box, hasLength(4));
      expect(r.box[0], const Offset(1.5, 2.5));
      expect(r.box[3], const Offset(7.5, 8.5));
    });

    test('accepts integer coordinates and promotes to double', () {
      final r = OcrResult.fromMap({
        'box': [
          [0, 0],
          [100, 0],
          [100, 30],
          [0, 30],
        ],
        'text': 'Int',
        'confidence': 1,
      });
      expect(r.box[1], const Offset(100, 0));
      expect(r.confidence, 1.0);
      // Confidence must be exactly a double after promotion.
      expect(r.confidence, isA<double>());
    });

    test('accepts mixed int/double within the same point list', () {
      final r = OcrResult.fromMap({
        'box': [
          [0, 1.25],
          [2, 3.5],
        ],
        'text': 'x',
        'confidence': 0.5,
      });
      expect(r.box[0], const Offset(0, 1.25));
      expect(r.box[1], const Offset(2, 3.5));
    });

    test('handles empty box list', () {
      final r = OcrResult.fromMap({
        'box': <dynamic>[],
        'text': 'empty',
        'confidence': 0.0,
      });
      expect(r.box, isEmpty);
      expect(r.text, 'empty');
    });

    test('handles 3-point and 5-point boxes without error', () {
      final r3 = OcrResult.fromMap({
        'box': [
          [0, 0],
          [1, 1],
          [2, 2],
        ],
        'text': 'a',
        'confidence': 0.1,
      });
      expect(r3.box, hasLength(3));

      final r5 = OcrResult.fromMap({
        'box': [
          [0, 0],
          [1, 0],
          [1, 1],
          [0, 1],
          [0, 0],
        ],
        'text': 'b',
        'confidence': 0.2,
      });
      expect(r5.box, hasLength(5));
    });

    test('preserves UTF-8 multibyte text as-is', () {
      final r = OcrResult.fromMap({
        'box': <dynamic>[],
        'text': '你好，世界 🚀 café',
        'confidence': 0.42,
      });
      expect(r.text, '你好，世界 🚀 café');
    });

    test('preserves embedded control characters', () {
      final r = OcrResult.fromMap({
        'box': <dynamic>[],
        'text': 'a\tb\nc',
        'confidence': 0.0,
      });
      expect(r.text, 'a\tb\nc');
    });
  });

  group('OcrResult.fromMap - defaults for missing fields', () {
    test('missing text defaults to empty string', () {
      final r = OcrResult.fromMap({
        'box': [
          [0, 0],
          [1, 1],
        ],
        'confidence': 0.5,
      });
      expect(r.text, '');
    });

    test('missing confidence defaults to 0.0', () {
      final r = OcrResult.fromMap({'box': <dynamic>[], 'text': 'x'});
      expect(r.confidence, 0.0);
    });

    test('null text coerces to empty string', () {
      final r = OcrResult.fromMap({
        'box': <dynamic>[],
        'text': null,
        'confidence': 0.3,
      });
      expect(r.text, '');
    });

    test('null confidence coerces to 0.0', () {
      final r = OcrResult.fromMap({
        'box': <dynamic>[],
        'text': 'x',
        'confidence': null,
      });
      expect(r.confidence, 0.0);
    });
  });

  group('OcrResult.fromMap - invalid input', () {
    test('missing box key throws', () {
      expect(
        () => OcrResult.fromMap({'text': 'x', 'confidence': 0.5}),
        throwsA(isA<TypeError>()),
      );
    });

    test('box not a list throws', () {
      expect(
        () => OcrResult.fromMap({
          'box': 'not-a-list',
          'text': 'x',
          'confidence': 0.5,
        }),
        throwsA(isA<TypeError>()),
      );
    });

    test('box point not a list throws', () {
      expect(
        () => OcrResult.fromMap({
          'box': ['x', 'y'],
          'text': 'oops',
          'confidence': 0.5,
        }),
        throwsA(isA<TypeError>()),
      );
    });

    test('box point coordinate not numeric throws', () {
      expect(
        () => OcrResult.fromMap({
          'box': [
            ['a', 'b'],
          ],
          'text': 'oops',
          'confidence': 0.5,
        }),
        throwsA(isA<TypeError>()),
      );
    });

    test('non-string text throws (cast failure)', () {
      expect(
        () => OcrResult.fromMap({
          'box': <dynamic>[],
          'text': 42,
          'confidence': 0.5,
        }),
        throwsA(isA<TypeError>()),
      );
    });

    test('non-numeric confidence throws (cast failure)', () {
      expect(
        () => OcrResult.fromMap({
          'box': <dynamic>[],
          'text': 'x',
          'confidence': 'high',
        }),
        throwsA(isA<TypeError>()),
      );
    });
  });

  group('OcrResult - const constructor & toString', () {
    test('const constructor stores fields verbatim', () {
      const r = OcrResult(
        box: [Offset(0, 0), Offset(1, 1)],
        text: 'k',
        confidence: 0.5,
      );
      expect(r.box, hasLength(2));
      expect(r.text, 'k');
      expect(r.confidence, 0.5);
    });

    test('toString contains text and confidence with 3 decimals', () {
      const r = OcrResult(box: [], text: 'Test', confidence: 0.5);
      final s = r.toString();
      expect(s, contains('Test'));
      expect(s, contains('0.500'));
    });

    test('toString handles empty text and 0.0 confidence', () {
      const r = OcrResult(box: [], text: '', confidence: 0.0);
      final s = r.toString();
      expect(s, contains('0.000'));
    });

    test('toString handles confidence above 1.0 (paranoid input)', () {
      const r = OcrResult(box: [], text: 'x', confidence: 1.5);
      expect(r.toString(), contains('1.500'));
    });
  });
}
