import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pp_ocr/pp_ocr.dart';
import 'package:pp_ocr/paddle_ocr_method_channel.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

/// 模拟的 PaddleOcr 平台实现
///
/// 使用 [MockPlatformInterfaceMixin] 来模拟平台接口，
/// 用于在测试中替代真实的平台实现，返回预设的固定值。
class MockPaddleOcrPlatform
    with MockPlatformInterfaceMixin
    implements PaddleOcrPlatform {
  @override
  Future<String?> getPlatformVersion() => Future.value('42');

  @override
  Future<bool> initialize({
    required String detModelPath,
    required String recModelPath,
    required String dictPath,
  }) => Future.value(true);

  @override
  Future<List<OcrResult>> recognizeImage(String imagePath) => Future.value([]);

  @override
  Future<List<OcrResult>> recognizeImageBytes(Uint8List imageBytes) =>
      Future.value([]);

  @override
  Future<String?> pickImage() => Future.value(null);
}

void main() {
  // 保存初始的平台实例，用于测试后恢复
  final PaddleOcrPlatform initialPlatform = PaddleOcrPlatform.instance;

  group('Platform interface', () {
    // 测试默认平台实例是否为 MethodChannelPaddleOcr
    test('$MethodChannelPaddleOcr is the default instance', () {
      expect(initialPlatform, isInstanceOf<MethodChannelPaddleOcr>());
    });

    // 测试平台接口不能用 implements 直接实现（会触发断言错误）
    test('Cannot be implemented with `implements`', () {
      expect(() {
        PaddleOcrPlatform.instance = _InvalidPlatform();
      }, throwsA(isA<AssertionError>()));
    });

    // 测试平台接口可以使用 MockPlatformInterfaceMixin 进行 mock
    test('Can be mocked with `MockPlatformInterfaceMixin`', () {
      final mock = MockPaddleOcrPlatform();
      PaddleOcrPlatform.instance = mock;
      expect(PaddleOcrPlatform.instance, mock);
      // 恢复默认平台实例
      PaddleOcrPlatform.instance = initialPlatform;
    });

    // 测试平台接口可以通过 extends 进行扩展
    test('Can be extended', () {
      final extended = _ExtendsPlatform();
      PaddleOcrPlatform.instance = extended;
      expect(PaddleOcrPlatform.instance, extended);
      // 恢复默认平台实例
      PaddleOcrPlatform.instance = initialPlatform;
    });
  });

  group('PaddleOcr API', () {
    late PaddleOcr ocr;
    late MockPaddleOcrPlatform mock;

    setUp(() {
      // 每个测试前创建新的 PaddleOcr 实例和 mock 平台
      ocr = PaddleOcr();
      mock = MockPaddleOcrPlatform();
      PaddleOcrPlatform.instance = mock;
    });

    tearDown(() {
      // 每个测试后恢复初始平台实例
      PaddleOcrPlatform.instance = initialPlatform;
    });

    // 测试 getPlatformVersion 返回平台版本字符串
    test('getPlatformVersion returns value from platform', () async {
      expect(await ocr.getPlatformVersion(), '42');
    });

    // 测试 initialize 成功时返回 true
    test('initialize returns true on success', () async {
      final result = await ocr.initialize(
        detModelPath: 'det.onnx',
        recModelPath: 'rec.onnx',
        dictPath: 'dict.txt',
      );
      expect(result, true);
    });

    // 测试 recognizeImage 返回 OcrResult 列表
    test('recognizeImage returns list of results', () async {
      final results = await ocr.recognizeImage('/path/to/image.png');
      expect(results, isA<List<OcrResult>>());
      expect(results, isEmpty);
    });

    // 测试 recognizeImageBytes 返回 OcrResult 列表
    test('recognizeImageBytes returns list of results', () async {
      final bytes = Uint8List.fromList([0x89, 0x50, 0x4E, 0x47]);
      final results = await ocr.recognizeImageBytes(bytes);
      expect(results, isA<List<OcrResult>>());
      expect(results, isEmpty);
    });

    // 测试 pickImage 在取消选择时返回 null
    test('pickImage returns null when cancelled', () async {
      final path = await ocr.pickImage();
      expect(path, isNull);
    });
  });
}

/// 无效的平台实现类
///
/// 仅使用 implements 实现 [PaddleOcrPlatform]，
/// 用于验证平台接口不允许直接 implements（会触发断言错误）。
class _InvalidPlatform implements PaddleOcrPlatform {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

/// 通过 extends 扩展的平台实现类
///
/// 用于验证平台接口允许通过 extends 进行扩展。
class _ExtendsPlatform extends PaddleOcrPlatform {
  @override
  Future<String?> getPlatformVersion() => Future.value('extended');

  @override
  Future<bool> initialize({
    required String detModelPath,
    required String recModelPath,
    required String dictPath,
  }) => Future.value(true);

  @override
  Future<List<OcrResult>> recognizeImage(String imagePath) => Future.value([]);

  @override
  Future<List<OcrResult>> recognizeImageBytes(Uint8List imageBytes) =>
      Future.value([]);

  @override
  Future<String?> pickImage() => Future.value(null);
}
