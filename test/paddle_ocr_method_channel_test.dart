import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:pp_ocr/paddle_ocr_method_channel.dart';
import 'package:pp_ocr/src/ocr_result.dart';

void main() {
  // 确保测试绑定已初始化
  TestWidgetsFlutterBinding.ensureInitialized();

  late MethodChannelPaddleOcr platform;
  // 定义与原生端通信的 MethodChannel
  const MethodChannel channel = MethodChannel('paddle_ocr');

  setUp(() {
    platform = MethodChannelPaddleOcr();
    // 设置 mock 的 MethodCall 处理器，模拟原生端返回的数据
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (MethodCall methodCall) async {
      switch (methodCall.method) {
        case 'getPlatformVersion':
          return 'Windows 10+';
        case 'initialize':
          return true;
        case 'pickImage':
          return '/path/to/image.png';
        case 'recognizeImage':
          // 返回两个模拟的识别结果（英文和中文）
          return <Map<String, dynamic>>[
            {
              'box': <List<double>>[
                [10.0, 20.0],
                [110.0, 20.0],
                [110.0, 50.0],
                [10.0, 50.0],
              ],
              'text': 'Hello World',
              'confidence': 0.95,
            },
            {
              'box': <List<double>>[
                [10.0, 60.0],
                [110.0, 60.0],
                [110.0, 90.0],
                [10.0, 90.0],
              ],
              'text': '你好世界',
              'confidence': 0.88,
            },
          ];
        case 'recognizeImageBytes':
          // 返回一个模拟的字节识别结果
          return <Map<String, dynamic>>[
            {
              'box': <List<double>>[
                [0.0, 0.0],
                [100.0, 0.0],
                [100.0, 30.0],
                [0.0, 30.0],
              ],
              'text': 'Test',
              'confidence': 0.75,
            },
          ];
        default:
          return null;
      }
    });
  });

  tearDown(() {
    // 清除 mock 处理器
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  group('MethodChannelPaddleOcr', () {
    // 测试 getPlatformVersion 返回平台版本字符串
    test('getPlatformVersion returns platform string', () async {
      expect(await platform.getPlatformVersion(), 'Windows 10+');
    });

    // 测试 initialize 返回 true
    test('initialize returns true', () async {
      final result = await platform.initialize(
        detModelPath: 'det.onnx',
        recModelPath: 'rec.onnx',
        dictPath: 'dict.txt',
      );
      expect(result, true);
    });

    // 测试 pickImage 返回文件路径
    test('pickImage returns file path', () async {
      final path = await platform.pickImage();
      expect(path, '/path/to/image.png');
    });

    // 测试 recognizeImage 正确解析返回的 OcrResult 列表
    test('recognizeImage returns parsed OcrResult list', () async {
      final results = await platform.recognizeImage('/path/to/image.png');
      expect(results.length, 2);

      // 验证第一个结果（英文文本）
      expect(results[0].text, 'Hello World');
      expect(results[0].confidence, closeTo(0.95, 0.001));
      expect(results[0].box.length, 4);
      expect(results[0].box[0].dx, 10.0);
      expect(results[0].box[0].dy, 20.0);
      expect(results[0].box[1].dx, 110.0);
      expect(results[0].box[2].dy, 50.0);

      // 验证第二个结果（中文文本）
      expect(results[1].text, '你好世界');
      expect(results[1].confidence, closeTo(0.88, 0.001));
    });

    // 测试 recognizeImageBytes 正确解析返回的 OcrResult 列表
    test('recognizeImageBytes returns parsed OcrResult list', () async {
      final bytes = Uint8List.fromList([0x89, 0x50, 0x4E, 0x47]);
      final results = await platform.recognizeImageBytes(bytes);
      expect(results.length, 1);
      expect(results[0].text, 'Test');
      expect(results[0].confidence, closeTo(0.75, 0.001));
    });
  });

  group('OcrResult parsing', () {
    // 测试 fromMap 正确解析 box、text 和 confidence
    test('fromMap parses box, text, and confidence', () {
      final result = OcrResult.fromMap({
        'box': [
          [1.0, 2.0],
          [3.0, 4.0],
          [5.0, 6.0],
          [7.0, 8.0],
        ],
        'text': 'OCR Text',
        'confidence': 0.123,
      });
      expect(result.text, 'OCR Text');
      expect(result.confidence, 0.123);
      expect(result.box.length, 4);
      expect(result.box[0].dx, 1.0);
      expect(result.box[0].dy, 2.0);
      expect(result.box[3].dx, 7.0);
      expect(result.box[3].dy, 8.0);
    });

    // 测试 fromMap 在缺少 text 字段时默认返回空字符串
    test('fromMap handles missing text (defaults to empty)', () {
      final result = OcrResult.fromMap({
        'box': [
          [0.0, 0.0],
          [1.0, 0.0],
          [1.0, 1.0],
          [0.0, 1.0],
        ],
        'confidence': 0.5,
      });
      expect(result.text, '');
      expect(result.confidence, 0.5);
    });

    // 测试 fromMap 在缺少 confidence 字段时默认返回 0.0
    test('fromMap handles missing confidence (defaults to 0.0)', () {
      final result = OcrResult.fromMap({
        'box': [
          [0.0, 0.0],
          [1.0, 0.0],
          [1.0, 1.0],
          [0.0, 1.0],
        ],
        'text': 'No confidence',
      });
      expect(result.text, 'No confidence');
      expect(result.confidence, 0.0);
    });

    // 测试 fromMap 能正确处理整数类型的 confidence 值
    test('fromMap handles integer confidence values', () {
      final result = OcrResult.fromMap({
        'box': [
          [0.0, 0.0],
        ],
        'text': '',
        'confidence': 1,
      });
      expect(result.confidence, 1.0);
    });

    // 测试 toString 包含文本和置信度信息
    test('toString contains text and confidence', () {
      const result = OcrResult(
        box: [],
        text: 'Test',
        confidence: 0.5,
      );
      final str = result.toString();
      expect(str, contains('Test'));
      expect(str, contains('0.500'));
    });
  });

  group('Edge cases', () {
    // 测试原生端返回 null 时 recognizeImage 返回空列表
    test('recognizeImage returns empty list when native returns null', () async {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (MethodCall call) async {
        return null;
      });

      final results = await platform.recognizeImage('/path/to/empty.png');
      expect(results, isEmpty);
    });

    // 测试原生端返回空字符串时 pickImage 返回空字符串
    test('pickImage returns empty string when native returns empty', () async {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (MethodCall call) async {
        return '';
      });

      final path = await platform.pickImage();
      expect(path, '');
    });

    // 测试原生端返回 false 时 initialize 返回 false
    test('initialize returns false when native returns false', () async {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, (MethodCall call) async {
        return false;
      });

      final result = await platform.initialize(
        detModelPath: 'det.onnx',
        recModelPath: 'rec.onnx',
        dictPath: 'dict.txt',
      );
      expect(result, false);
    });
  });
}
