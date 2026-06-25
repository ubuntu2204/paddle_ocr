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

  test('$MethodChannelPaddleOcr is the default instance', () {
    expect(initialPlatform, isInstanceOf<MethodChannelPaddleOcr>());
  });

  test('getPlatformVersion', () async {
    PaddleOcr paddleOcrPlugin = PaddleOcr();
    MockPaddleOcrPlatform fakePlatform = MockPaddleOcrPlatform();
    PaddleOcrPlatform.instance = fakePlatform;

    expect(await paddleOcrPlugin.getPlatformVersion(), '42');
  });

  test('initialize', () async {
    PaddleOcr paddleOcrPlugin = PaddleOcr();
    MockPaddleOcrPlatform fakePlatform = MockPaddleOcrPlatform();
    PaddleOcrPlatform.instance = fakePlatform;

    final result = await paddleOcrPlugin.initialize(
      detModelPath: 'det.onnx',
      recModelPath: 'rec.onnx',
      dictPath: 'dict.txt',
    );
    expect(result, true);
  });
}
