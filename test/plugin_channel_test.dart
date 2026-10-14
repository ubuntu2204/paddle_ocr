import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pp_ocr/paddle_ocr_plugin_channel.dart';
import 'package:pp_ocr/src/ocr_result.dart';

/// 插件通道（文件对话框 / 平台信息）测试。
///
/// 识别链路 100% 走 FFI、不经过本通道，因此这里只验证两件事：
/// - pickImage / getPlatformVersion 的参数与返回值处理正确；
/// - OcrResult 的通用行为（toString 等）。
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const MethodChannel channel = MethodChannel('paddle_ocr');
  late PluginChannelPaddleOcr platform;

  setUp(() {
    platform = PluginChannelPaddleOcr();
  });

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  group('PluginChannelPaddleOcr', () {
    test('pickImage returns file path', () async {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (MethodCall call) async {
        expect(call.method, 'pickImage');
        return '/path/to/image.png';
      });
      expect(await platform.pickImage(), '/path/to/image.png');
    });

    test('pickImage null means cancelled', () async {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (_) async => null);
      expect(await platform.pickImage(), isNull);
    });

    test('pickImage rethrows PlatformException', () async {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (_) async {
        throw PlatformException(code: 'PICKER_FAILED', message: 'no dialog');
      });
      await expectLater(
        platform.pickImage(),
        throwsA(isA<PlatformException>()
            .having((e) => e.code, 'code', 'PICKER_FAILED')),
      );
    });

    test('getPlatformVersion returns platform string', () async {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (MethodCall call) async {
        expect(call.method, 'getPlatformVersion');
        return 'Linux 6.5';
      });
      expect(await platform.getPlatformVersion(), 'Linux 6.5');
    });

    test('getPlatformVersion propagates MissingPluginException', () async {
      // 不安装 mock handler。
      await expectLater(
        platform.getPlatformVersion(),
        throwsA(isA<MissingPluginException>()),
      );
    });
  });

  group('OcrResult', () {
    test('toString contains text and confidence', () {
      const result = OcrResult(box: [], text: 'Test', confidence: 0.5);
      final str = result.toString();
      expect(str, contains('Test'));
      expect(str, contains('0.500'));
    });
  });
}
