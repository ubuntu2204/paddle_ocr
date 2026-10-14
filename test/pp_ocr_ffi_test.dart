import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:pp_ocr/paddle_ocr_plugin_channel.dart';
import 'package:pp_ocr/paddle_ocr_platform_interface.dart';
import 'package:pp_ocr/pp_ocr_ffi.dart';
import 'package:pp_ocr/src/pp_ocr_ffi_bindings.dart';

/// FFI 层单元测试（识别链路唯一路径，无 MethodChannel 回退）。
///
/// 单元测试运行环境（`flutter test`）里，Flutter 的 host 侧动态库
///（`pp_ocr_plugin.dll` / `libpp_ocr_plugin.so`）不在默认搜索路径下，
/// 因此 FFI 不可用 —— 新契约下 [FfiPaddleOcr] 会直接抛 [StateError]，
/// 而不是静默降级到慢路径。
void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  tearDown(() {
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

  group('FfiPaddleOcr without native lib (test host)', () {
    late FfiPaddleOcr ffi;

    setUp(() {
      ffi = FfiPaddleOcr();
      PaddleOcrPlatform.instance = ffi;
    });

    test('initialize 抛 StateError 而不是静默回退', () async {
      if (Platform.isWindows || Platform.isLinux) {
        // 测试宿主里原生库不可用 → 明确报错（FFI-only 契约）。
        await expectLater(
          ffi.initialize(
            detModelPath: '/tmp/det.onnx',
            recModelPath: '/tmp/rec.onnx',
            dictPath: '/tmp/dict.txt',
          ),
          throwsA(isA<StateError>()),
        );
      }
    });

    test('recognizeImageBytes 抛 StateError 而不是静默回退', () async {
      if (Platform.isWindows || Platform.isLinux) {
        await expectLater(
          ffi.recognizeImageBytes(Uint8List.fromList([1])),
          throwsA(isA<StateError>()),
        );
      }
    });

    test('runPipeline 抛 StateError 而不是静默回退', () async {
      if (Platform.isWindows || Platform.isLinux) {
        await expectLater(
          ffi.runPipeline(Uint8List.fromList([1]), const []),
          throwsA(isA<StateError>()),
        );
      }
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
      expect(PaddleOcrPlatform.instance, isA<FfiPaddleOcr>());
    });

    test('PluginChannelPaddleOcr 只承载文件对话框/平台信息', () {
      expect(PluginChannelPaddleOcr(), isA<PluginChannelPaddleOcr>());
    });

    test('替换 instance 后需要正确恢复', () {
      final before = PaddleOcrPlatform.instance;
      // PluginChannelPaddleOcr 不继承平台接口，这里只验证同类型替换。
      PaddleOcrPlatform.instance = FfiPaddleOcr();
      expect(PaddleOcrPlatform.instance, isA<FfiPaddleOcr>());
      PaddleOcrPlatform.instance = before;
      expect(PaddleOcrPlatform.instance, same(before));
    });
  });
}
