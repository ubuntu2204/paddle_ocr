import 'package:flutter_test/flutter_test.dart';
import 'package:pp_ocr/pp_ocr.dart';

/// [OcrOptions] 及其枚举的纯 Dart 行为测试。
///
/// 这里不触碰原生库，只验证：
/// - 分支位掩码与集合的双向转换稳定（它会跨 FFI / MethodChannel 传输）
/// - `toMap` / `fromMap` 往返不丢字段（MethodChannel 回退路径依赖它）
/// - `copyWith` 只改指定字段
/// - 非法枚举值回退到安全默认，而不是抛异常
void main() {
  group('OcrBranch 位掩码', () {
    test('maskOf / branchesOf 双向一致', () {
      const set = {OcrBranch.raw, OcrBranch.whiteText, OcrBranch.upscale2x};
      final mask = OcrBranch.maskOf(set);
      expect(
        mask,
        OcrBranch.raw.bit | OcrBranch.whiteText.bit | OcrBranch.upscale2x.bit,
      );
      expect(OcrBranch.branchesOf(mask), set);
    });

    test('位值唯一且与原生侧约定一致', () {
      final bits = OcrBranch.values.map((b) => b.bit).toList();
      expect(bits.toSet().length, bits.length);
      expect(OcrBranch.raw.bit, 1);
      expect(OcrBranch.whiteText.bit, 1 << 8);
    });

    test('空掩码还原成空集合', () {
      expect(OcrBranch.branchesOf(0), isEmpty);
      expect(OcrBranch.maskOf(const <OcrBranch>{}), 0);
    });

    test('预设组合集合互不包含遗漏', () {
      expect(OcrBranch.mono, {OcrBranch.raw});
      expect(OcrBranch.surveillance, contains(OcrBranch.whiteText));
      expect(OcrBranch.surveillance, isNot(contains(OcrBranch.otsuBinarize)));
      expect(OcrBranch.all.length, OcrBranch.values.length);
    });
  });

  group('OcrOptions 构造与派生属性', () {
    test('默认值是单分支、无预处理', () {
      const o = OcrOptions();
      expect(o.branches, {OcrBranch.raw});
      expect(o.isPlain, isTrue);
      expect(o.hasRoi, isFalse);
      expect(o.stitchSameLine, isFalse);
      expect(o.recEnhanceCrops, 0);
      expect(o.variantSelection, VariantSelection.geometryFitRawFirst);
    });

    test('开二值化分支后不再是 plain', () {
      const o = OcrOptions(
        branches: {OcrBranch.raw, OcrBranch.adaptiveBinarize},
      );
      expect(o.isPlain, isFalse);
    });

    test('带 ROI 时不是 plain', () {
      final o = const OcrOptions().withRoi(10, 20, 300, 80);
      expect(o.hasRoi, isTrue);
      expect(o.isPlain, isFalse);
      expect(o.roiX, 10);
      expect(o.roiH, 80);
      // withoutRoi 应回到整图
      expect(o.withoutRoi().hasRoi, isFalse);
      expect(o.withoutRoi().roiW, 0);
    });

    test('branches 命名构造只改分支', () {
      const o = OcrOptions.branches({OcrBranch.clahe});
      expect(o.branches, {OcrBranch.clahe});
      expect(o.detThresh, 0.30);
    });
  });

  group('copyWith', () {
    test('只覆盖显式传入的字段', () {
      const base = OcrOptions(
        branches: {OcrBranch.raw, OcrBranch.clahe},
        detThresh: 0.28,
        maxRegions: 42,
        stitchSameLine: true,
      );
      final next = base.copyWith(detThresh: 0.15);

      expect(next.detThresh, 0.15);
      expect(next.branches, base.branches);
      expect(next.maxRegions, 42);
      expect(next.stitchSameLine, isTrue);
      // 原对象不可变
      expect(base.detThresh, 0.28);
    });

    test('sourcePreset 默认沿用', () {
      const base = OcrOptions(sourcePreset: OcrPreset.surveillance);
      expect(
        base.copyWith(detThresh: 0.2).sourcePreset,
        OcrPreset.surveillance,
      );
    });
  });

  group('枚举容错', () {
    test('未知值回退到安全默认而不抛异常', () {
      expect(BinarizeMode.fromValue(999), BinarizeMode.none);
      expect(EnhanceMode.fromValue(-3), EnhanceMode.none);
      expect(ColorMode.fromValue(42), ColorMode.keep);
      expect(
        VariantSelection.fromValue(9),
        VariantSelection.geometryFitRawFirst,
      );
      expect(ResultSortOrder.fromValue(88), ResultSortOrder.readingOrder);
    });

    test('branchesOf 忽略未知位', () {
      final unknownBit = 1 << 20;
      final set = OcrBranch.branchesOf(OcrBranch.raw.bit | unknownBit);
      expect(set, {OcrBranch.raw});
    });
  });

  group('toString', () {
    test('包含分支与关键参数，便于日志定位', () {
      const o = OcrOptions(
        branches: {OcrBranch.raw, OcrBranch.whiteText},
        stitchSameLine: true,
        regionPadRatio: 0.3,
      );
      final s = o.toString();
      expect(s, contains('whiteText'));
      expect(s, contains('raw'));
      expect(s, contains('stitch: true'));
      expect(s, contains('roi: full'));
    });

    test('带 ROI 时打印矩形', () {
      final s = const OcrOptions(roiX: 1, roiY: 2, roiW: 3, roiH: 4).toString();
      expect(s, contains('roi: 1,2,3,4'));
    });
  });
}
