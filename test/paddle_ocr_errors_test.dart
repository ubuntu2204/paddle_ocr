import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pp_ocr/paddle_ocr_method_channel.dart';

/// MethodChannel 层的错误路径与调用参数正确性测试。
///
/// 覆盖：
/// - 原生端抛出 [PlatformException] 时，Dart 侧要按原样 rethrow
/// - `MissingPluginException`（未注册处理器）能正常向上抛
/// - 各 API 传给原生的 arguments 结构和 key 命名保持一致
/// - 处理器返回不当类型（例如给 String 槽位返回 int）时行为可预期
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const MethodChannel channel = MethodChannel('paddle_ocr');
  late MethodChannelPaddleOcr platform;
  late List<MethodCall> log;

  setUp(() {
    platform = MethodChannelPaddleOcr();
    log = <MethodCall>[];
  });

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  void mockHandler(Future<Object?> Function(MethodCall) handler) {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall call) async {
          log.add(call);
          return handler(call);
        });
  }

  group('PlatformException propagation', () {
    test(
      'initialize rethrows PlatformException with original code/message',
      () async {
        mockHandler(
          (_) async => throw PlatformException(
            code: 'INIT_FAILED',
            message: 'Model file missing',
            details: 'det.onnx',
          ),
        );
        await expectLater(
          platform.initialize(
            detModelPath: 'det.onnx',
            recModelPath: 'rec.onnx',
            dictPath: 'dict.txt',
          ),
          throwsA(
            isA<PlatformException>()
                .having((e) => e.code, 'code', 'INIT_FAILED')
                .having((e) => e.message, 'message', 'Model file missing')
                .having((e) => e.details, 'details', 'det.onnx'),
          ),
        );
      },
    );

    test('recognizeImage rethrows NOT_INITIALIZED error', () async {
      mockHandler(
        (_) async => throw PlatformException(
          code: 'NOT_INITIALIZED',
          message: 'call initialize() first',
        ),
      );
      await expectLater(
        platform.recognizeImage('/some/x.png'),
        throwsA(
          isA<PlatformException>().having(
            (e) => e.code,
            'code',
            'NOT_INITIALIZED',
          ),
        ),
      );
    });

    test('recognizeImageBytes rethrows INVALID_ARGS error', () async {
      mockHandler(
        (_) async => throw PlatformException(
          code: 'INVALID_ARGS',
          message: 'imageBytes is required',
        ),
      );
      await expectLater(
        platform.recognizeImageBytes(Uint8List.fromList([1])),
        throwsA(
          isA<PlatformException>().having(
            (e) => e.code,
            'code',
            'INVALID_ARGS',
          ),
        ),
      );
    });

    test('pickImage rethrows underlying PlatformException', () async {
      mockHandler(
        (_) async => throw PlatformException(
          code: 'PICKER_DISMISSED',
          message: 'user cancelled',
        ),
      );
      await expectLater(
        platform.pickImage(),
        throwsA(
          isA<PlatformException>().having(
            (e) => e.message,
            'message',
            'user cancelled',
          ),
        ),
      );
    });
  });

  group('Missing handler (no native side)', () {
    test(
      'initialize throws MissingPluginException when handler not set',
      () async {
        // 不安装 mock handler。
        await expectLater(
          platform.initialize(
            detModelPath: 'det.onnx',
            recModelPath: 'rec.onnx',
            dictPath: 'dict.txt',
          ),
          throwsA(isA<MissingPluginException>()),
        );
      },
    );

    test('getPlatformVersion throws MissingPluginException', () async {
      await expectLater(
        platform.getPlatformVersion(),
        throwsA(isA<MissingPluginException>()),
      );
    });
  });

  group('Argument shape', () {
    test('initialize sends det/rec/dict keys under exact names', () async {
      mockHandler((_) async => true);
      await platform.initialize(
        detModelPath: '/tmp/d.onnx',
        recModelPath: '/tmp/r.onnx',
        dictPath: '/tmp/d.txt',
      );
      expect(log.single.method, 'initialize');
      final args = log.single.arguments as Map;
      expect(args.keys.toSet(), {'detModelPath', 'recModelPath', 'dictPath'});
      expect(args['detModelPath'], '/tmp/d.onnx');
      expect(args['recModelPath'], '/tmp/r.onnx');
      expect(args['dictPath'], '/tmp/d.txt');
    });

    test('recognizeImage sends imagePath key', () async {
      mockHandler((_) async => <Map<String, dynamic>>[]);
      await platform.recognizeImage('/tmp/photo.png');
      expect(log.single.method, 'recognizeImage');
      final args = log.single.arguments as Map;
      expect(args.keys.single, 'imagePath');
      expect(args['imagePath'], '/tmp/photo.png');
    });

    test('recognizeImageBytes sends imageBytes as Uint8List', () async {
      mockHandler((_) async => <Map<String, dynamic>>[]);
      final bytes = Uint8List.fromList(List.generate(16, (i) => i));
      await platform.recognizeImageBytes(bytes);
      expect(log.single.method, 'recognizeImageBytes');
      final args = log.single.arguments as Map;
      expect(args.keys.single, 'imageBytes');
      expect(args['imageBytes'], isA<Uint8List>());
      expect((args['imageBytes'] as Uint8List), bytes);
    });

    test('pickImage is invoked with null arguments', () async {
      mockHandler((_) async => '/tmp/picked.png');
      await platform.pickImage();
      expect(log.single.method, 'pickImage');
      expect(log.single.arguments, isNull);
    });

    test('getPlatformVersion is invoked with null arguments', () async {
      mockHandler((_) async => 'Linux 6.5');
      await platform.getPlatformVersion();
      expect(log.single.method, 'getPlatformVersion');
      expect(log.single.arguments, isNull);
    });
  });

  group('Native return-type drift', () {
    test('initialize casts non-bool null result to false (default)', () async {
      mockHandler((_) async => null);
      final r = await platform.initialize(
        detModelPath: 'a',
        recModelPath: 'b',
        dictPath: 'c',
      );
      expect(r, false);
    });

    test(
      'recognizeImage with unexpected String list throws TypeError',
      () async {
        mockHandler((_) async => <String>['a', 'b']);
        // _parseResults 将 String 强转为 Map，会抛 TypeError；确认行为可预期。
        await expectLater(
          platform.recognizeImage('/x.png'),
          throwsA(isA<TypeError>()),
        );
      },
    );

    test('pickImage null → Dart null (user cancelled)', () async {
      mockHandler((_) async => null);
      final p = await platform.pickImage();
      expect(p, isNull);
    });

    test('getPlatformVersion null → Dart null', () async {
      mockHandler((_) async => null);
      final v = await platform.getPlatformVersion();
      expect(v, isNull);
    });
  });

  group('Multiple calls in sequence', () {
    test('each API hits the channel exactly once', () async {
      mockHandler((call) async {
        switch (call.method) {
          case 'initialize':
            return true;
          case 'recognizeImage':
            return <Map<String, dynamic>>[];
          case 'pickImage':
            return '/tmp/x.png';
          case 'getPlatformVersion':
            return 'Linux';
          default:
            return null;
        }
      });
      await platform.initialize(
        detModelPath: 'a',
        recModelPath: 'b',
        dictPath: 'c',
      );
      await platform.recognizeImage('/tmp/a.png');
      await platform.pickImage();
      await platform.getPlatformVersion();
      expect(log.map((c) => c.method).toList(), [
        'initialize',
        'recognizeImage',
        'pickImage',
        'getPlatformVersion',
      ]);
    });
  });
}
