import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pp_ocr/paddle_ocr_method_channel.dart';
import 'package:pp_ocr/pp_ocr.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

/// [PaddleOcr] 顶层 API 的行为测试。
///
/// 覆盖：
/// - 平台 instance 延迟获取，多次调用一致
/// - `dispose()` 释放后，再次使用会重新读取当前 instance
/// - `dispose()` 对非 FFI 平台 instance 是安全的空操作
/// - 各 API 参数原样透传给底层平台
class _RecorderPlatform
    with MockPlatformInterfaceMixin
    implements PaddleOcrPlatform {
  final List<String> calls = <String>[];
  Object? nextInitializeResult;
  List<OcrResult> nextRecognizeResult = const <OcrResult>[];
  String? nextPickImageResult;
  String? nextPlatformVersion;

  @override
  Future<bool> initialize({
    required String detModelPath,
    required String recModelPath,
    required String dictPath,
  }) async {
    calls.add('initialize:$detModelPath|$recModelPath|$dictPath');
    return nextInitializeResult as bool? ?? true;
  }

  @override
  Future<List<OcrResult>> recognizeImage(String imagePath) async {
    calls.add('recognizeImage:$imagePath');
    return nextRecognizeResult;
  }

  @override
  Future<List<OcrResult>> recognizeImageBytes(Uint8List imageBytes) async {
    calls.add('recognizeImageBytes:${imageBytes.length}');
    return nextRecognizeResult;
  }

  @override
  Future<String?> pickImage() async {
    calls.add('pickImage');
    return nextPickImageResult;
  }

  @override
  Future<String?> getPlatformVersion() async {
    calls.add('getPlatformVersion');
    return nextPlatformVersion;
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  late PaddleOcrPlatform originalInstance;
  late _RecorderPlatform recorder;

  setUp(() {
    originalInstance = PaddleOcrPlatform.instance;
    recorder = _RecorderPlatform();
    PaddleOcrPlatform.instance = recorder;
  });

  tearDown(() {
    PaddleOcrPlatform.instance = originalInstance;
  });

  group('PaddleOcr API delegation', () {
    test('initialize passes all three paths through', () async {
      final ocr = PaddleOcr();
      final ok = await ocr.initialize(
        detModelPath: '/a/det.onnx',
        recModelPath: '/a/rec.onnx',
        dictPath: '/a/dict.txt',
      );
      expect(ok, true);
      expect(recorder.calls, [
        'initialize:/a/det.onnx|/a/rec.onnx|/a/dict.txt',
      ]);
    });

    test('initialize returns false when platform returns false', () async {
      recorder.nextInitializeResult = false;
      final ocr = PaddleOcr();
      final ok = await ocr.initialize(
        detModelPath: 'a',
        recModelPath: 'b',
        dictPath: 'c',
      );
      expect(ok, false);
    });

    test('recognizeImage forwards imagePath', () async {
      recorder.nextRecognizeResult = const <OcrResult>[
        OcrResult(box: [], text: 'x', confidence: 0.5),
      ];
      final ocr = PaddleOcr();
      final res = await ocr.recognizeImage('/p/1.png');
      expect(res, hasLength(1));
      expect(res.first.text, 'x');
      expect(recorder.calls.single, 'recognizeImage:/p/1.png');
    });

    test('recognizeImageBytes forwards byte length', () async {
      final ocr = PaddleOcr();
      await ocr.recognizeImageBytes(Uint8List.fromList([1, 2, 3]));
      expect(recorder.calls.single, 'recognizeImageBytes:3');
    });

    test('pickImage null propagates', () async {
      recorder.nextPickImageResult = null;
      final ocr = PaddleOcr();
      expect(await ocr.pickImage(), isNull);
      expect(recorder.calls.single, 'pickImage');
    });

    test('getPlatformVersion returns string', () async {
      recorder.nextPlatformVersion = '42';
      final ocr = PaddleOcr();
      expect(await ocr.getPlatformVersion(), '42');
    });
  });

  group('PaddleOcr.dispose lifecycle', () {
    test('dispose on a non-FFI platform is a safe no-op', () async {
      final ocr = PaddleOcr();
      // 触发一次调用以填充 _platform 缓存
      await ocr.getPlatformVersion();
      expect(() => ocr.dispose(), returnsNormally);
      // dispose 后 _platform 被清空；再次调用应重新读取 instance。
      await ocr.getPlatformVersion();
      // recorder 至少被调用两次（第一次缓存前 + 第二次 dispose 之后）
      expect(recorder.calls.where((c) => c == 'getPlatformVersion').length, 2);
    });

    test(
      'dispose clears cached platform reference so later swaps take effect',
      () async {
        final ocr = PaddleOcr();
        await ocr.getPlatformVersion();
        expect(recorder.calls, contains('getPlatformVersion'));

        ocr.dispose();

        final second = _RecorderPlatform();
        PaddleOcrPlatform.instance = second;
        await ocr.getPlatformVersion();
        // 第二次调用应打到新的 recorder 上。
        expect(second.calls, contains('getPlatformVersion'));
      },
    );

    test('multiple dispose calls are safe', () {
      final ocr = PaddleOcr();
      ocr.dispose();
      ocr.dispose();
      ocr.dispose();
    });
  });

  group('PaddleOcr lazy instance', () {
    test('instance is read at first API call, not at construction', () async {
      // 构造 PaddleOcr 不应用任何 recorder 调用。
      final ocr = PaddleOcr();
      expect(recorder.calls, isEmpty);
      // 首次调用触发实例读取。
      await ocr.getPlatformVersion();
      expect(recorder.calls, isNotEmpty);
    });

    test('instance is captured once and reused across calls', () async {
      final ocr = PaddleOcr();
      await ocr.getPlatformVersion();
      await ocr.getPlatformVersion();
      // 中途换掉全局 instance，已缓存的 ocr 仍走 recorder。
      final swap = _RecorderPlatform();
      PaddleOcrPlatform.instance = swap;
      await ocr.getPlatformVersion();
      expect(recorder.calls.where((c) => c == 'getPlatformVersion').length, 3);
      expect(swap.calls, isEmpty);
    });
  });

  // 与真实 MethodChannel 的兼容性冒烟（确保当 instance 是
  // MethodChannelPaddleOcr 时也能被上层正常持有）。
  test('MethodChannelPaddleOcr remains a valid platform subclass', () {
    expect(MethodChannelPaddleOcr(), isA<PaddleOcrPlatform>());
  });
}
