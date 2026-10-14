import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pp_ocr/src/ocr_pipeline.dart';
import 'package:pp_ocr/src/ocr_preprocessor.dart';

/// 管线纯逻辑测试（不触碰原生库）：
/// - 文本 em 宽度估算与原生侧权重一致
/// - 择优打分：几何契合主导、原像素加分、同分高分辨率候选优先
/// - 预处理插件接口的默认值契约
void main() {
  group('estimateTextWidthInEm', () {
    test('CJK 每字 1.0 em', () {
      expect(estimateTextWidthInEm('篮球场'), closeTo(3.0, 1e-9));
    });

    test('ASCII 字母数字 0.6、标点空格 0.35', () {
      expect(estimateTextWidthInEm('G66'), closeTo(1.8, 1e-9));
      expect(estimateTextWidthInEm('- '), closeTo(0.7, 1e-9));
    });

    test('混合串与直觉一致', () {
      // 一舍篮球场东北路口球G66-5724：10 CJK + 7 字母数字×0.6 + 标点 0.35
      expect(
        estimateTextWidthInEm('一舍篮球场东北路口球G66-5724'),
        closeTo(14.55, 0.01),
      );
    });
  });

  group('scoreVariant', () {
    const long = 300.0;
    const short = 22.0;

    test('fill 接近 1 的候选胜出', () {
      final good = scoreVariant('一舍篮球场东北路口球G66-5724', 1.0, long, short,
          true, 1.0);
      final fewer =
          scoreVariant('篮球场东北路口球G66-5724', 1.0, long, short, true, 1.0);
      expect(good, greaterThan(fewer));
    });

    test('同几何时原像素候选加分', () {
      final raw = scoreVariant('同文', 1.0, long, short, true, 1.0);
      final enh = scoreVariant('同文', 1.0, long, short, false, 1.0);
      expect(raw, greaterThan(enh));
    });

    test('同分时高分辨率候选优先（平局裁决）', () {
      final v1 = scoreVariant('一合篮球场东北路口球G66-5724', 1.0, long, short,
          true, 1.0);
      final v3 = scoreVariant('一舍篮球场东北路口球G66-5724', 1.0, long, short,
          true, 3.0);
      // 两个候选 em 相同（籃/篮都算 CJK），raw bonus 相同 → 分数本应持平；
      // scale 裁决项让放大分支胜出。
      final v1Tie =
          scoreVariant('一舍篮球场东北路口球G66-5724', 1.0, long, short, true, 1.0);
      expect(v3, greaterThan(v1Tie));
      // 几何差异仍主导：明显更差的候选即便高分源也不该赢。
      expect(v1, lessThan(v3));
    });
  });

  group('OcrPreprocessor contract', () {
    test('NativeTransformPreprocessor 默认为识别源、原尺寸', () {
      const p = NativeTransformPreprocessor(
        OcrTransformParams(),
        name: 't',
      );
      expect(p.scale, 1.0);
      expect(p.recSource, isTrue);
    });

    test('upscalePreprocessor 的 scale 与参数一致', () {
      final p = upscalePreprocessor(times: 3);
      expect(p.scale, 3.0);
      expect(p.name, 'upscale3x');
      expect(p.params.scalePercent, 300);
    });

    test('binarizePreprocessor 不作识别源', () {
      final p = binarizePreprocessor();
      expect(p.recSource, isFalse);
    });

    test('whiteTextPreprocessor 不作识别源', () {
      final p = whiteTextPreprocessor();
      expect(p.recSource, isFalse);
      expect(p.params.enhanceMode.value, 6); // kEnhanceWhiteText
    });

    test('CustomTransformPreprocessor 透传变换函数', () {
      final p = CustomTransformPreprocessor(
        (bytes) => Uint8List.fromList(bytes.reversed.toList()),
        name: 'custom',
        scale: 2.0,
      );
      final out = p.process(Uint8List.fromList([1, 2, 3]));
      expect(out, [3, 2, 1]);
      expect(p.scale, 2.0);
      expect(p.recSource, isTrue);
    });
  });
}
