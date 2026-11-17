import 'package:test/test.dart';

import 'package:pp_ocr/pp_ocr.dart';
// 与被测代码同源的 Offset：Flutter 下取 dart:ui，纯 Dart 下取轻量替身。
import 'package:pp_ocr/src/dart_offset.dart'
    if (dart.library.ui) 'dart:ui'
    show Offset;

void main() {
  test('OcrResult 在纯 Dart 运行时可用，不依赖 Flutter 引擎', () {
    const point = Offset(12, 34);
    final result = OcrResult(
      box: [point, point, point, point],
      text: 'hello',
      confidence: 0.9,
    );

    expect(result.box, hasLength(4));
    expect(result.box.first.dx, 12);
    expect(result.box.first.dy, 34);
    expect(result.text, 'hello');
    expect(result.confidence, 0.9);
  });

  test('PaddleOcr 可实例化，懒初始化不会提前加载原生库', () {
    final ocr = PaddleOcr();
    expect(ocr, isA<PaddleOcr>());
    // 没有访问过 _impl，dispose 也不会触发 FFI。
    ocr.dispose();
  });
}
