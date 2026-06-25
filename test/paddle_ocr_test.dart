import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pp_ocr/pp_ocr.dart';
import 'package:pp_ocr/paddle_ocr_method_channel.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

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
  final PaddleOcrPlatform initialPlatform = PaddleOcrPlatform.instance;

  group('Platform interface', () {
    test('$MethodChannelPaddleOcr is the default instance', () {
      expect(initialPlatform, isInstanceOf<MethodChannelPaddleOcr>());
    });

    test('Cannot be implemented with `implements`', () {
      expect(() {
        PaddleOcrPlatform.instance = _InvalidPlatform();
      }, throwsA(isA<AssertionError>()));
    });

    test('Can be mocked with `MockPlatformInterfaceMixin`', () {
      final mock = MockPaddleOcrPlatform();
      PaddleOcrPlatform.instance = mock;
      expect(PaddleOcrPlatform.instance, mock);
      // Restore default
      PaddleOcrPlatform.instance = initialPlatform;
    });

    test('Can be extended', () {
      final extended = _ExtendsPlatform();
      PaddleOcrPlatform.instance = extended;
      expect(PaddleOcrPlatform.instance, extended);
      // Restore default
      PaddleOcrPlatform.instance = initialPlatform;
    });
  });

  group('PaddleOcr API', () {
    late PaddleOcr ocr;
    late MockPaddleOcrPlatform mock;

    setUp(() {
      ocr = PaddleOcr();
      mock = MockPaddleOcrPlatform();
      PaddleOcrPlatform.instance = mock;
    });

    tearDown(() {
      PaddleOcrPlatform.instance = initialPlatform;
    });

    test('getPlatformVersion returns value from platform', () async {
      expect(await ocr.getPlatformVersion(), '42');
    });

    test('initialize returns true on success', () async {
      final result = await ocr.initialize(
        detModelPath: 'det.onnx',
        recModelPath: 'rec.onnx',
        dictPath: 'dict.txt',
      );
      expect(result, true);
    });

    test('recognizeImage returns list of results', () async {
      final results = await ocr.recognizeImage('/path/to/image.png');
      expect(results, isA<List<OcrResult>>());
      expect(results, isEmpty);
    });

    test('recognizeImageBytes returns list of results', () async {
      final bytes = Uint8List.fromList([0x89, 0x50, 0x4E, 0x47]);
      final results = await ocr.recognizeImageBytes(bytes);
      expect(results, isA<List<OcrResult>>());
      expect(results, isEmpty);
    });

    test('pickImage returns null when cancelled', () async {
      final path = await ocr.pickImage();
      expect(path, isNull);
    });
  });
}

class _InvalidPlatform implements PaddleOcrPlatform {
  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

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
