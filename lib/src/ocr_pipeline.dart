import 'dart:ffi';
import 'dart:typed_data';
import 'dart:ui';

import 'package:ffi/ffi.dart';

import 'ocr_options.dart';
import 'ocr_preprocessor.dart';
import 'ocr_result.dart';
import 'pp_ocr_ffi_bindings.dart';

/// 多变体择优时的候选来源。
class _Variant {
  const _Variant(this.bytes, this.scale, this.recSource, this.label);
  final Uint8List bytes;
  final double scale;
  final bool recSource;
  final String label;
}

/// 估算 UTF-8 文本按「字高」为单位的排版宽度（与原生侧
/// EstimateTextWidthInEm 同一套权重：CJK 1.0、ASCII 字母数字 0.6、
/// 空格标点 0.35、其余 0.7）。
double estimateTextWidthInEm(String text) {
  var em = 0.0;
  for (final rune in text.runes) {
    final cp = rune;
    if (cp == 0x20) {
      em += 0.35;
    } else if (cp >= 0x20 && cp <= 0x7E) {
      final alnum = (cp >= 0x30 && cp <= 0x39) ||
          (cp >= 0x41 && cp <= 0x5A) ||
          (cp >= 0x61 && cp <= 0x7A);
      em += alnum ? 0.60 : 0.35;
    } else if ((cp >= 0x3000 && cp <= 0x30FF) ||
        (cp >= 0x3400 && cp <= 0x4DBF) ||
        (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xAC00 && cp <= 0xD7AF) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0x20000 && cp <= 0x3134F)) {
      em += 1.0;
    } else {
      em += 0.7;
    }
  }
  return em;
}

/// 多变体择优打分（与原生 VariantSelectionScore(mode=4) 同款公式，
/// 附加「同分时高分辨率候选优先」的裁决项）。
double scoreVariant(
  String text,
  double confidence,
  double longSide,
  double shortSide,
  bool rawCrop,
  double scale,
) {
  final len = text.runes.length;
  final expectedEm = longSide / (shortSide < 1.0 ? 1.0 : shortSide);
  double geometry;
  if (expectedEm <= 0.5) {
    geometry = len * (confidence < 0.05 ? 0.05 : confidence);
  } else {
    final actualEm = estimateTextWidthInEm(text);
    final fill = actualEm / expectedEm;
    final deviation = (fill - 1.0).abs();
    final penalty = deviation > 5.0 ? 5.0 : deviation;
    geometry = (5.0 - penalty) + 0.05 * confidence + 0.001 * len;
  }
  const rawCropBonus = 2.0;
  return geometry + (rawCrop ? rawCropBonus : 0.0) + 0.001 * scale;
}

/// 在已初始化的原生引擎上执行「宿主编排管线」：
///
///   1. 底图作为 raw 变体，加上宿主注入的各路预处理变体；
///   2. 每路变体跑文本检测（放大变体的 detMaxSideLen 自动同步放宽），
///      框坐标换算回底图；
///   3. 合并多路框为「识别窗」（去重 + 同行拼接 + 成员基底外扩）；
///   4. 每个窗在底图 + 各识别源变体上分别识别，几何契合度择优。
///
/// [engine] 必须已经过 initialize。
List<OcrResult> runOcrPipeline({
  required PpOcrFfiBindings bindings,
  required Pointer<Void> engine,
  required Uint8List imageBytes,
  required List<OcrPreprocessor> preprocessors,
  required OcrOptions options,
}) {
  // ---- 1. 生成变体 ----
  final variants = <_Variant>[
    _Variant(imageBytes, 1.0, true, 'raw'),
  ];
  for (final p in preprocessors) {
    if (p is NativeTransformPreprocessor) {
      final paramsPtr = calloc<PpOcrTransformParamsC>();
      try {
        p.params.writeTo(paramsPtr.ref);
        final out = bindings.preprocessBytes(engine, imageBytes, paramsPtr);
        if (out != null) {
          final data = out.data.asTypedList(out.size);
          variants.add(_Variant(
            Uint8List.fromList(data),
            p.scale,
            p.recSource,
            p.name,
          ));
        }
      } finally {
        calloc.free(paramsPtr);
      }
    } else {
      final out = p.process(imageBytes);
      if (out.isNotEmpty) {
        variants.add(_Variant(out, p.scale, p.recSource, p.name));
      }
    }
  }

  // ---- 2. 每路变体检测（框换算回底图坐标）----
  final allBoxes = <({List<double> box, int branch, double score})>[];
  int baseW = 0;
  int baseH = 0;
  final optsPtr = options.toNativeStruct();
  try {
    for (final v in variants) {
      var variantOptions = options;
      Pointer<PpOcrOptionsC> voPtr = optsPtr;
      final ownsPtr = v.scale != 1.0;
      if (ownsPtr) {
        // 放大变体的输入边长同步放宽，否则检测器又把它缩回去。
        variantOptions = options.copyWith(
          detMaxSideLen: (options.detMaxSideLen * v.scale).round(),
        );
        voPtr = variantOptions.toNativeStruct();
      }
      try {
        final detected = bindings.detectBytes(engine, v.bytes, voPtr);
        if (detected == null) continue;
        if (v.scale == 1.0) {
          baseW = detected.imageWidth;
          baseH = detected.imageHeight;
        }
        for (int i = 0; i < detected.count; i++) {
          final item = detected.items[i];
          final box = List<double>.generate(
              8, (j) => item.box[j] / (v.scale == 1.0 ? 1.0 : v.scale));
          allBoxes.add((
            box: box,
            branch: item.branchIndex,
            score: item.score,
          ));
        }
      } finally {
        if (ownsPtr) calloc.free(voPtr);
      }
    }

    // ---- 3. 合并为识别窗 ----
    if (baseW <= 0 || baseH <= 0) {
      // 没有任何变体检出（例如整图无文字）：返回空。
      return const [];
    }
    final boxesPtr = calloc<PpOcrBoxInputC>(allBoxes.length);
    try {
      for (int i = 0; i < allBoxes.length; i++) {
        final b = allBoxes[i].box;
        for (int j = 0; j < 8; j++) {
          boxesPtr[i].box[j] = b[j];
        }
        boxesPtr[i].branchIndex = allBoxes[i].branch;
        boxesPtr[i].score = allBoxes[i].score;
      }
      final merged = bindings.mergeBoxes(
        engine,
        boxesPtr,
        allBoxes.length,
        optsPtr,
        baseW,
        baseH,
      );
      if (merged == null || merged.count == 0) return const [];

      // ---- 4a. 按变体批量识别 ----
      // 一次 FFI 调用识别全部窗（原生侧线程池并行），而不是「窗 × 变体」
      // 各调一次。结果按 [变体下标][窗下标] 收集；打分逐窗独立，改批量
      // 不改变任何选择结果。
      final perVariant =
          List<List<({String text, double confidence})>>.generate(
        variants.length,
        (_) => List.filled(merged.count, (text: '', confidence: 0.0)),
      );
      for (int vi = 0; vi < variants.length; vi++) {
        final v = variants[vi];
        if (!v.recSource) continue;
        final boxesPtr = calloc<Double>(merged.count * 8);
        try {
          for (int r = 0; r < merged.count; r++) {
            final window = merged.items[r];
            for (int j = 0; j < 8; j++) {
              boxesPtr[r * 8 + j] = window.box[j] * v.scale;
            }
          }
          final batch = bindings.recognizeBoxesBytes(
            engine,
            v.bytes,
            boxesPtr,
            merged.count,
            optsPtr,
          );
          if (batch == null) continue;
          for (int r = 0; r < batch.length && r < merged.count; r++) {
            perVariant[vi][r] = batch[r];
          }
        } finally {
          calloc.free(boxesPtr);
        }
      }

      // ---- 4b. 逐窗择优 ----
      final results = <OcrResult>[];
      for (int r = 0; r < merged.count; r++) {
        final window = merged.items[r];
        final box8 = List<double>.generate(8, (j) => window.box[j]);
        // 打分几何：窗长边 + 成员统计字高不可用时退回窗短边。
        // 与原生管线一致：窗就是裁剪边界，expected 用窗几何。
        double xs0 = double.infinity, ys0 = double.infinity;
        double xs1 = double.negativeInfinity, ys1 = double.negativeInfinity;
        for (int j = 0; j < 4; j++) {
          xs0 = xs0 < window.box[j * 2] ? xs0 : window.box[j * 2];
          ys0 = ys0 < window.box[j * 2 + 1] ? ys0 : window.box[j * 2 + 1];
          xs1 = xs1 > window.box[j * 2] ? xs1 : window.box[j * 2];
          ys1 = ys1 > window.box[j * 2 + 1] ? ys1 : window.box[j * 2 + 1];
        }
        final longSide = (xs1 - xs0).abs() > (ys1 - ys0).abs()
            ? xs1 - xs0
            : ys1 - ys0;
        final shortSide = (xs1 - xs0).abs() > (ys1 - ys0).abs()
            ? ys1 - ys0
            : xs1 - xs0;

        String bestText = '';
        var bestConf = 0.0;
        var bestScore = double.negativeInfinity;
        for (int vi = 0; vi < variants.length; vi++) {
          final v = variants[vi];
          if (!v.recSource) continue;
          final out = perVariant[vi][r];
          if (out.text.isEmpty) continue;
          final score = scoreVariant(
            out.text,
            out.confidence,
            longSide * v.scale,
            shortSide * v.scale,
            v.scale == 1.0,
            v.scale,
          );
          if (score > bestScore) {
            bestScore = score;
            bestText = out.text;
            bestConf = out.confidence;
          }
        }
        if (bestText.isEmpty) continue;

        results.add(OcrResult(
          box: [
            for (int j = 0; j < 4; j++)
              Offset(box8[j * 2], box8[j * 2 + 1]),
          ],
          text: bestText,
          confidence: bestConf,
        ));
      }
      return results;
    } finally {
      calloc.free(boxesPtr);
    }
  } finally {
    calloc.free(optsPtr);
  }
}
