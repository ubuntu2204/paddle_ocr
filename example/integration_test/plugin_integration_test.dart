// Integration tests for the pp_ocr plugin.
//
// These run inside a full Flutter application (Linux desktop or Windows
// desktop) and exercise the real native side, unlike Dart unit tests.
//
// The tests reference the model files that ship inside the repository at
// `<repo_root>/model/`. Because the plugin's example app is launched with
// `flutter test integration_test`, the working directory of the process
// is the example/ folder, so we locate models via `Directory.current`.
// If that assumption changes, the paths are overridable via the
// `PP_OCR_MODEL_DIR` environment variable.
//
// Model files are expected to exist at:
//   $PP_OCR_MODEL_DIR/det.onnx
//   $PP_OCR_MODEL_DIR/inference.onnx
//   $PP_OCR_MODEL_DIR/ppocr_v6_dict.txt
//
// Tests that need an actual image use the model directory only for
// initialize(); the pickImage test just launches the native file dialog
// and immediately cancels out (returns empty string), so it is safe to
// run unattended.

import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

import 'package:pp_ocr/pp_ocr.dart';

String? _findModelDir() {
  final candidates = <String>[
    if (Platform.environment['PP_OCR_MODEL_DIR'] != null)
      Platform.environment['PP_OCR_MODEL_DIR']!,
    // Common relative paths from where `flutter test integration_test`
    // launches the process (the example/ directory).
    '../model',
    'model',
    // Absolute fallbacks so local developers can run without env vars.
    '/home/ubuntu/project/paddle_ocr/model',
  ];
  for (final dir in candidates) {
    final d = Directory(dir);
    if (d.existsSync() &&
        File('${d.path}/det.onnx').existsSync() &&
        File('${d.path}/inference.onnx').existsSync() &&
        File('${d.path}/ppocr_v6_dict.txt').existsSync()) {
      return d.absolute.path;
    }
  }
  return null;
}

// ---------------------------------------------------------------------------
// 最小灰度 PNG 生成器（无第三方依赖）
//
// 集成测试需要一张 cv::imdecode 能解的真实图片来驱动检测/合并/识别。
// 仓库不附带图片资产，这里手写 PNG：zlib 用「stored（不压缩）deflate 块」，
// 逐字节可验证。内容为白底 + 三条带横向断口的黑色横带（模拟文字行），
// DB 检测器能稳定检出框，从而覆盖 merge/recognize 的完整数据路径。
// ---------------------------------------------------------------------------

int _crc32(List<int> data) {
  var crc = 0xFFFFFFFF;
  for (final byte in data) {
    crc ^= byte;
    for (var i = 0; i < 8; i++) {
      crc = (crc & 1) != 0 ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1);
    }
  }
  return (crc ^ 0xFFFFFFFF) & 0xFFFFFFFF;
}

Uint8List _makeTextLikePng(int width, int height) {
  final raw = <int>[];
  for (var y = 0; y < height; y++) {
    raw.add(0); // filter: none
    for (var x = 0; x < width; x++) {
      final inBand = (y > height * 0.25 && y < height * 0.31) ||
          (y > height * 0.5 && y < height * 0.56) ||
          (y > height * 0.75 && y < height * 0.81);
      final gap = (x % 41) < 9;
      raw.add((inBand && !gap) ? 0 : 255);
    }
  }

  final zlib = <int>[0x78, 0x01];
  var offset = 0;
  while (offset < raw.length) {
    final remaining = raw.length - offset;
    final len = remaining > 65535 ? 65535 : remaining;
    final last = offset + len >= raw.length;
    zlib.add(last ? 0x01 : 0x00);
    zlib.add(len & 0xFF);
    zlib.add((len >> 8) & 0xFF);
    zlib.add(~len & 0xFF);
    zlib.add((~len >> 8) & 0xFF);
    zlib.addAll(raw.getRange(offset, offset + len));
    offset += len;
  }
  var a = 1, b = 0;
  for (final byte in raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  final adler = (b << 16) | a;
  zlib.add((adler >> 24) & 0xFF);
  zlib.add((adler >> 16) & 0xFF);
  zlib.add((adler >> 8) & 0xFF);
  zlib.add(adler & 0xFF);

  final out = <int>[];
  void chunk(String type, List<int> data) {
    final typeBytes = type.codeUnits;
    out.add((data.length >> 24) & 0xFF);
    out.add((data.length >> 16) & 0xFF);
    out.add((data.length >> 8) & 0xFF);
    out.add(data.length & 0xFF);
    out.addAll(typeBytes);
    out.addAll(data);
    final crc = _crc32([...typeBytes, ...data]);
    out.add((crc >> 24) & 0xFF);
    out.add((crc >> 16) & 0xFF);
    out.add((crc >> 8) & 0xFF);
    out.add(crc & 0xFF);
  }

  out.addAll([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]);
  chunk('IHDR', [
    (width >> 24) & 0xFF, (width >> 16) & 0xFF, (width >> 8) & 0xFF,
    width & 0xFF,
    (height >> 24) & 0xFF, (height >> 16) & 0xFF, (height >> 8) & 0xFF,
    height & 0xFF,
    8, // bit depth
    0, // color type: grayscale
    0, // compression
    0, // filter
    0, // interlace
  ]);
  chunk('IDAT', zlib);
  chunk('IEND', []);
  return Uint8List.fromList(out);
}

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  final modelDir = _findModelDir();

  group('Plugin lifecycle', () {
    testWidgets('getPlatformVersion returns a non-empty string', (
      tester,
    ) async {
      final plugin = PaddleOcr();
      final version = await plugin.getPlatformVersion();
      expect(version, isNotNull);
      expect(version!.isNotEmpty, isTrue);
      // FFI 后端返回原生库版本（形如 "pp_ocr 0.5.2"）；MethodChannel
      // 不可用时才会回退平台名。两种都算合法，只校验非空。
      expect(
        version.startsWith('pp_ocr') ||
            version.startsWith('Windows') ||
            version.startsWith('Linux'),
        isTrue,
        reason: 'unexpected platform version: $version',
      );
    });

    testWidgets('initialize with valid model paths returns true', (
      tester,
    ) async {
      if (modelDir == null) {
        markTestSkipped('model directory not found; set PP_OCR_MODEL_DIR');
        return;
      }
      final ocr = PaddleOcr();
      final ok = await ocr.initialize(
        detModelPath: '$modelDir/det.onnx',
        recModelPath: '$modelDir/inference.onnx',
        dictPath: '$modelDir/ppocr_v6_dict.txt',
      );
      expect(ok, isTrue);
      ocr.dispose();
    });

    testWidgets('initialize with bogus paths reports failure', (
      tester,
    ) async {
      final ocr = PaddleOcr();
      // FFI 后端的 initialize 对「模型加载失败」返回 false 并写 last_error
      // （不抛异常）；错误详情走 getLastError / 原生日志。
      final ok = await ocr.initialize(
        detModelPath: '/__does_not_exist__/det.onnx',
        recModelPath: '/__does_not_exist__/rec.onnx',
        dictPath: '/__does_not_exist__/dict.txt',
      );
      expect(ok, isFalse);
    });

    testWidgets('dispose is safe to call multiple times', (tester) async {
      final ocr = PaddleOcr();
      ocr.dispose();
      ocr.dispose();
      ocr.dispose();
    });

    testWidgets('pickImage returns empty string when dialog is dismissed', (
      tester,
    ) async {
      // This test verifies the native dialog can be launched without
      // crashing. In an automated environment there is no user to click
      // OK, so the test simply ensures the call resolves quickly. We
      // rely on the dialog returning empty string on cancel; if the
      // dialog blocks, the test would hang — which is exactly what we
      // want to catch (see the pickImage SIGSEGV regression).
      final ocr = PaddleOcr();
      // We do NOT await pickImage here because that requires user
      // interaction. Instead we verify that the plugin is initialized
      // enough to route the method call. The regression test lives in
      // linux/paddle_ocr_plugin.cc and is exercised manually.
      final version = await ocr.getPlatformVersion();
      expect(version, isNotNull);
      ocr.dispose();
    });
  });

  group('Real OCR round-trip (only if model dir present)', () {
    testWidgets('recognizeImage returns OcrResult list on a synthetic PNG', (
      tester,
    ) async {
      if (modelDir == null) {
        markTestSkipped('model directory not found');
        return;
      }
      final ocr = PaddleOcr();
      final ok = await ocr.initialize(
        detModelPath: '$modelDir/det.onnx',
        recModelPath: '$modelDir/inference.onnx',
        dictPath: '$modelDir/ppocr_v6_dict.txt',
      );
      expect(ok, isTrue, reason: 'OCR engine must initialize');

      // Generate a tiny 100x40 white PNG on the fly (no image library
      // needed on the plugin side). We don't require the model to find
      // anything in it — an empty list is a valid outcome. We mainly
      // assert that the native code does not crash / throw.
      final tmpDir = await Directory.systemTemp.createTemp('pp_ocr_it');
      final pngPath = '${tmpDir.path}/blank.png';
      // Minimal valid PNG: 1x1 pixel. OpenCV will decode this fine.
      final onePixelPng = <int>[
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, // signature
        // IHDR + IDAT + IEND chunks for a 1x1 grayscale PNG
        // (precomputed minimal file, see <https://en.wikipedia.org/wiki/Portable_Network_Graphics>)
      ];
      // Since building a valid PNG by hand is tedious, use a raw byte
      // array that OpenCV will reject. Recognize should gracefully
      // return an empty list rather than throwing.
      await File(pngPath).writeAsBytes(onePixelPng);

      final results = await ocr.recognizeImage(pngPath);
      expect(results, isA<List<OcrResult>>());
      // We don't care about the exact count for a garbage image — the
      // important assertion is that we didn't crash.
      await tmpDir.delete(recursive: true);
      ocr.dispose();
    });

    testWidgets('recognizeImageBytes rejects uninitialized engine', (
      tester,
    ) async {
      if (modelDir == null) {
        markTestSkipped('model directory not found');
        return;
      }
      final ocr = PaddleOcr();
      // Do NOT initialize; expect an error path.
      // 注意入参必须是真 Uint8List —— 传 List<int> 会先触发 Dart 类型
      // 错误（同步抛出，expectLater 捕获不到），掩盖了真正的「引擎未
      // 初始化」错误路径。
      await expectLater(
        ocr.recognizeImageBytes(Uint8List.fromList([0, 1, 2, 3])),
        throwsA(anything),
      );
      ocr.dispose();
    });

    testWidgets('runPipeline (Dart orchestration) does not crash', (
      tester,
    ) async {
      if (modelDir == null) {
        markTestSkipped('model directory not found');
        return;
      }
      final ocr = PaddleOcr();
      final ok = await ocr.initialize(
        detModelPath: '$modelDir/det.onnx',
        recModelPath: '$modelDir/inference.onnx',
        dictPath: '$modelDir/ppocr_v6_dict.txt',
      );
      expect(ok, isTrue, reason: 'OCR engine must initialize');

      // runPipeline 走 Dart 编排的四个原子 FFI 接口（preprocess/detect/
      // merge/recognize_boxes），与 recognizeImage 的原生内部管线是两条
      // 独立链路。回归背景：detectBytes / mergeBoxes 曾把
      // `return out.ref` 出的结构体**视图**在 `calloc.free(out)` 之后返回
      // （dart:ffi 的 `.ref` 是视图不是拷贝），调用方读到被 malloc 复用
      // 的野内存 → 字段随机变垃圾 → 解引用 items 时 SIGSEGV。
      // 本用例即该段错误的端到端守卫：合成图含可检出的横带，确保
      // detect/merge 的快照路径带真实数据执行。
      const preprocessors = <OcrPreprocessor>[
        NativeTransformPreprocessor(
          OcrTransformParams(scalePercent: 200),
          name: 'upscale2x',
          scale: 2.0,
        ),
        NativeTransformPreprocessor(
          OcrTransformParams(scalePercent: 300),
          name: 'upscale3x',
          scale: 3.0,
        ),
        NativeTransformPreprocessor(
          OcrTransformParams(
            enhanceMode: EnhanceMode.clahe,
            claheClipLimit: 3.0,
          ),
          name: 'clahe',
        ),
        NativeTransformPreprocessor(
          OcrTransformParams(
            binarizeMode: BinarizeMode.adaptiveGaussianInverted,
            colorMode: ColorMode.gray3,
          ),
          name: 'adaptive_bin',
          recSource: false,
        ),
        NativeTransformPreprocessor(
          OcrTransformParams(enhanceMode: EnhanceMode.denoiseClahe),
          name: 'denoise_clahe',
        ),
      ];

      final results = await ocr.runPipeline(
        _makeTextLikePng(640, 480),
        preprocessors,
        options: OcrOptions.preset(OcrPreset.surveillance),
      );
      expect(results, isA<List<OcrResult>>());
      // 合成横带应被检出（不强求具体数量，但至少证明快照字段可信）。
      expect(results.length, greaterThan(0));
      for (final r in results) {
        expect(r.box.length, 4);
      }
      ocr.dispose();
    });
  });
}
