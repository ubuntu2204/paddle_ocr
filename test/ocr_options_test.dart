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

  group('toMap / fromMap 往返', () {
    test('全字段往返保持一致', () {
      const original = OcrOptions(
        detThresh: 0.24,
        detBoxThresh: 0.41,
        detUnclipRatio: 1.75,
        detMaxSideLen: 1280,
        detMinBoxSize: 5,
        detMinBoxArea: 120,
        recImgHeight: 64,
        recMaxImgWidth: 3000,
        recEnhanceCrops: 2,
        recTryAllBranches: false,
        recMinConfidence: 0.35,
        variantSelection: VariantSelection.geometryFit,
        cropBinarizeMode: BinarizeMode.adaptiveMeanInverted,
        cropAdaptiveBlockSize: 21,
        cropAdaptiveC: 7.5,
        cropEnhanceMode: EnhanceMode.whiteText,
        cropClaheClipLimit: 3.5,
        cropClaheGridSize: 12,
        cropSharpenAmount: 1.4,
        cropMorphCloseKernel: 2,
        cropMorphOpenKernel: 1,
        cropColorMode: ColorMode.gray3,
        branches: {OcrBranch.raw, OcrBranch.upscale2x, OcrBranch.whiteText},
        mergeIouThreshold: 0.22,
        mergeContainThreshold: 0.66,
        stitchSameLine: true,
        lineMinYOverlap: 0.5,
        lineMaxGapRatio: 0.9,
        lineMaxHeightDiffRatio: 0.4,
        lineMaxWidthRatio: 5.0,
        regionPadRatio: 0.3,
        maxRegions: 77,
        sortOrder: ResultSortOrder.areaDescending,
        roiX: 8,
        roiY: 16,
        roiW: 640,
        roiH: 48,
        verboseStats: true,
        sourcePreset: OcrPreset.lowContrast,
      );

      final restored = OcrOptions.fromMap(original.toMap());

      expect(restored.detThresh, original.detThresh);
      expect(restored.detBoxThresh, original.detBoxThresh);
      expect(restored.detUnclipRatio, original.detUnclipRatio);
      expect(restored.detMaxSideLen, original.detMaxSideLen);
      expect(restored.detMinBoxSize, original.detMinBoxSize);
      expect(restored.detMinBoxArea, original.detMinBoxArea);
      expect(restored.recImgHeight, original.recImgHeight);
      expect(restored.recMaxImgWidth, original.recMaxImgWidth);
      expect(restored.recEnhanceCrops, original.recEnhanceCrops);
      expect(restored.recTryAllBranches, original.recTryAllBranches);
      expect(restored.recMinConfidence, original.recMinConfidence);
      expect(restored.variantSelection, original.variantSelection);
      expect(restored.cropBinarizeMode, original.cropBinarizeMode);
      expect(restored.cropAdaptiveBlockSize, original.cropAdaptiveBlockSize);
      expect(restored.cropAdaptiveC, original.cropAdaptiveC);
      expect(restored.cropEnhanceMode, original.cropEnhanceMode);
      expect(restored.cropClaheClipLimit, original.cropClaheClipLimit);
      expect(restored.cropClaheGridSize, original.cropClaheGridSize);
      expect(restored.cropSharpenAmount, original.cropSharpenAmount);
      expect(restored.cropMorphCloseKernel, original.cropMorphCloseKernel);
      expect(restored.cropMorphOpenKernel, original.cropMorphOpenKernel);
      expect(restored.cropColorMode, original.cropColorMode);
      expect(restored.branches, original.branches);
      expect(restored.mergeIouThreshold, original.mergeIouThreshold);
      expect(restored.mergeContainThreshold, original.mergeContainThreshold);
      expect(restored.stitchSameLine, original.stitchSameLine);
      expect(restored.lineMinYOverlap, original.lineMinYOverlap);
      expect(restored.lineMaxGapRatio, original.lineMaxGapRatio);
      expect(restored.lineMaxHeightDiffRatio, original.lineMaxHeightDiffRatio);
      expect(restored.lineMaxWidthRatio, original.lineMaxWidthRatio);
      expect(restored.regionPadRatio, original.regionPadRatio);
      expect(restored.maxRegions, original.maxRegions);
      expect(restored.sortOrder, original.sortOrder);
      expect(restored.roiX, original.roiX);
      expect(restored.roiY, original.roiY);
      expect(restored.roiW, original.roiW);
      expect(restored.roiH, original.roiH);
      expect(restored.verboseStats, original.verboseStats);
      expect(restored.sourcePreset, original.sourcePreset);
    });

    test('toMap 的布尔量压成 0/1，便于原生侧按 int32 读取', () {
      const o = OcrOptions(stitchSameLine: true, verboseStats: true);
      final m = o.toMap();
      expect(m['stitchSameLine'], 1);
      expect(m['verboseStats'], 1);
      expect(m['recTryAllBranches'], 1);
      expect(m['branches'], OcrBranch.maskOf(o.branches));
    });

    test('fromMap 空 map 得到标准默认值', () {
      final o = OcrOptions.fromMap(const <Object?, Object?>{});
      expect(o.detThresh, 0.30);
      expect(o.branches, {OcrBranch.raw});
      expect(o.variantSelection, VariantSelection.geometryFitRawFirst);
      expect(o.isPlain, isTrue);
    });

    test('fromMap 容忍类型漂移（double 传成 int 等）', () {
      final o = OcrOptions.fromMap(const <Object?, Object?>{
        'detThresh': 0, // int 而非 double
        'maxRegions': 12.0, // double 而非 int
        'unknownKey': 1,
      });
      expect(o.detThresh, 0.0);
      expect(o.maxRegions, 12);
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
