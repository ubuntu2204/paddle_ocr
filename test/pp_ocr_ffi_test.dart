import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pp_ocr/paddle_ocr_method_channel.dart';
import 'package:pp_ocr/paddle_ocr_platform_interface.dart';
import 'package:pp_ocr/pp_ocr_ffi.dart';
import 'package:pp_ocr/src/pp_ocr_ffi_bindings.dart';

/// FFI 层与回退逻辑的单元测试。
///
/// 单元测试运行环境（`flutter test`）里，Flutter 的 host 侧动态库
///（`pp_ocr_plugin.dll` / `libpp_ocr_plugin.so`）不在默认搜索路径下，
/// 因此 [FfiPaddleOcr] 会走"FFI 不可用 → 回退 [MethodChannelPaddleOcr]"
/// 这条路径。这正好是我们要覆盖的核心行为。
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const MethodChannel channel = MethodChannel('paddle_ocr');
  late List<MethodCall> log;
  late Map<String, Object?> responses;

  setUp(() {
    log = <MethodCall>[];
    responses = <String, Object?>{};
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall call) async {
          log.add(call);
          final r = responses[call.method];
          if (r is PlatformException) throw r;
          return r;
        });
  });

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
    // 恢复默认平台实例，避免污染其他测试文件。
    PaddleOcrPlatform.instance = FfiPaddleOcr();
  });

  group('PpOcrFfiBindings._loadLibrary behavior in test host', () {
    test('Windows/Linux 宿主：DynamicLibrary.open 抛异常（未部署插件）', () {
      if (!Platform.isWindows && !Platform.isLinux) {
        // 其它宿主上会走 UnsupportedError 分支，跳过。
        return;
      }
      // 在测试环境下，插件 .so/.dll 未被 dlopen 搜索路径命中，
      // PpOcrFfiBindings.instance 应抛出（ ArgumentError / dlopen fail）。
      expect(() => PpOcrFfiBindings.instance, throwsA(isA<Object>()));
    });

    test('macOS/其它平台会给出 UnsupportedError 提示（文档化行为）', () {
      // 该测试仅在非 Win/Linux 的开发者机器上有意义。
      if (Platform.isWindows || Platform.isLinux) return;
      expect(() => PpOcrFfiBindings.instance, throwsA(isA<UnsupportedError>()));
    });
  });

  group('FfiPaddleOcr falls back to MethodChannel when native lib absent', () {
    late FfiPaddleOcr ffi;

    setUp(() {
      ffi = FfiPaddleOcr();
      PaddleOcrPlatform.instance = ffi;
    });

    test('initialize routes to MethodChannel handler', () async {
      responses['initialize'] = true;
      final ok = await ffi.initialize(
        detModelPath: '/tmp/det.onnx',
        recModelPath: '/tmp/rec.onnx',
        dictPath: '/tmp/dict.txt',
      );
      expect(ok, true);
      expect(log, hasLength(1));
      expect(log.single.method, 'initialize');
      final args = log.single.arguments as Map;
      expect(args['detModelPath'], '/tmp/det.onnx');
      expect(args['recModelPath'], '/tmp/rec.onnx');
      expect(args['dictPath'], '/tmp/dict.txt');
    });

    test('recognizeImage routes and parses results', () async {
      responses['recognizeImage'] = <Map<String, dynamic>>[
        {
          'box': [
            [0.0, 0.0],
            [10.0, 0.0],
            [10.0, 5.0],
            [0.0, 5.0],
          ],
          'text': 'hi',
          'confidence': 0.9,
        },
      ];
      final results = await ffi.recognizeImage('/tmp/x.png');
      expect(results, hasLength(1));
      expect(results.first.text, 'hi');
      expect(results.first.confidence, closeTo(0.9, 1e-6));
      expect(log.single.method, 'recognizeImage');
    });

    test('recognizeImageBytes forwards Uint8List argument', () async {
      responses['recognizeImageBytes'] = <Map<String, dynamic>>[];
      final bytes = Uint8List.fromList([1, 2, 3, 4, 5]);
      final results = await ffi.recognizeImageBytes(bytes);
      expect(results, isEmpty);
      expect(log.single.method, 'recognizeImageBytes');
      final args = log.single.arguments as Map;
      expect(args['imageBytes'], isA<Uint8List>());
      expect((args['imageBytes'] as Uint8List).length, 5);
    });

    test(
      'pickImage ALWAYS goes through MethodChannel (even when FFI would)',
      () async {
        responses['pickImage'] = '/tmp/picked.png';
        final path = await ffi.pickImage();
        expect(path, '/tmp/picked.png');
        expect(log.single.method, 'pickImage');
      },
    );

    test('getPlatformVersion falls back to MethodChannel string', () async {
      responses['getPlatformVersion'] = 'Linux 6.5';
      final v = await ffi.getPlatformVersion();
      expect(v, 'Linux 6.5');
      expect(log, hasLength(1));
    });

    test('dispose is safe to call before any initialize', () {
      expect(() => ffi.dispose(), returnsNormally);
    });

    test('dispose is idempotent', () {
      ffi.dispose();
      expect(() => ffi.dispose(), returnsNormally);
    });
  });

  group('PaddleOcrPlatform.instance contract', () {
    test('default instance is FfiPaddleOcr', () {
      // 默认应为 FFI 实现，FFI 内部再回退到 MethodChannel。
      expect(PaddleOcrPlatform.instance, isA<FfiPaddleOcr>());
    });

    test('MethodChannelPaddleOcr 是 PaddleOcrPlatform 的子类', () {
      expect(MethodChannelPaddleOcr(), isA<PaddleOcrPlatform>());
    });

    test('替换 instance 后需要正确恢复', () {
      final before = PaddleOcrPlatform.instance;
      final mc = MethodChannelPaddleOcr();
      PaddleOcrPlatform.instance = mc;
      expect(PaddleOcrPlatform.instance, same(mc));
      PaddleOcrPlatform.instance = before;
      expect(PaddleOcrPlatform.instance, same(before));
    });
  });
}
