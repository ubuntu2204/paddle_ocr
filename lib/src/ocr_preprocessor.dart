import 'dart:typed_data';

import 'pp_ocr_ffi_bindings.dart';
import 'ocr_options.dart';

/// 预处理变换参数（对应原生 `PpOcrTransformParams`）。
///
/// 这是内置算子（缩放/增强/二值化/形态学/通道）的参数包；执行顺序固定为
/// 缩放 → 增强 → 二值化 → 形态学 → 通道处理，与完整管线的分支一致。
class OcrTransformParams {
  const OcrTransformParams({
    this.scalePercent = 100,
    this.binarizeMode = BinarizeMode.none,
    this.adaptiveBlockSize = 0,
    this.adaptiveC = 10.0,
    this.enhanceMode = EnhanceMode.none,
    this.claheClipLimit = 2.0,
    this.claheGridSize = 8,
    this.sharpenAmount = 1.0,
    this.morphOpenKernel = 0,
    this.morphCloseKernel = 0,
    this.colorMode = ColorMode.keep,
  });

  /// 缩放百分比：100 = 原尺寸，300 = 放大 3 倍。
  final int scalePercent;
  final BinarizeMode binarizeMode;

  /// 自适应阈值邻域（奇数像素；0 = 按图尺寸自动取）。
  final int adaptiveBlockSize;
  final double adaptiveC;
  final EnhanceMode enhanceMode;
  final double claheClipLimit;
  final int claheGridSize;
  final double sharpenAmount;

  /// 形态学开/闭运算核半径（0 = 关闭）。
  final int morphOpenKernel;
  final int morphCloseKernel;
  final ColorMode colorMode;

  /// 写入 C 结构体。调用方负责分配/释放。
  void writeTo(PpOcrTransformParamsC s) {
    s.structSize = PpOcrFfiBindings.instance.transformParamsSize();
    s.scalePercent = scalePercent;
    s.binarizeMode = binarizeMode.value;
    s.adaptiveBlockSize = adaptiveBlockSize;
    s.adaptiveC = adaptiveC;
    s.enhanceMode = enhanceMode.value;
    s.claheClipLimit = claheClipLimit;
    s.claheGridSize = claheGridSize;
    s.sharpenAmount = sharpenAmount;
    s.morphOpenKernel = morphOpenKernel;
    s.morphCloseKernel = morphCloseKernel;
    s.colorMode = colorMode.value;
  }
}

/// 预处理插件接口。
///
/// 这是 pp_ocr 面向宿主 App 的核心扩展点：完整识别接口
/// （[PaddleOcr.recognizeImage]）固定使用内置分支；宿主想要注入自己的
/// 预处理（超分、色彩域提取、去雾、目标区域增强……）时，实现本接口并
/// 把实例交给 [PaddleOcr.runPipeline]，即可与引擎共享同一套
/// 检测 / 合并 / 择优识别核心。
///
/// 两种实现方式：
///   * [NativeTransformPreprocessor]：参数驱动的内置 OpenCV 算子，
///     无需写任何图像代码；
///   * 自定义子类：覆盖 [process]，用任意 Dart 图像库产出变体图。
abstract class OcrPreprocessor {
  const OcrPreprocessor();

  /// 预处理器名（日志与诊断用）。
  String get name;

  /// 变体图相对输入图的缩放系数（2x 放大填 2.0）。管线用它把检测框
  /// 换算回底图坐标，并在同分时优先高分辨率候选。
  double get scale => 1.0;

  /// 变体图能否作为识别源。二值化/白字提取类输出已丢失自然灰度分布，
  /// 喂给识别器会产生乱码，应返回 false（它只负责「把框找全」）。
  bool get recSource => true;

  /// 对输入图（PNG/JPEG 编码字节）做变换，返回变体图（编码字节）。
  Uint8List process(Uint8List imageBytes);
}

/// 内置算子预处理器：参数驱动，交由原生 OpenCV 变换执行。
class NativeTransformPreprocessor extends OcrPreprocessor {
  const NativeTransformPreprocessor(
    this.params, {
    this.name = 'transform',
    this.scale = 1.0,
    this.recSource = true,
  });

  /// 变换参数。
  final OcrTransformParams params;

  @override
  final String name;

  @override
  final double scale;

  @override
  final bool recSource;

  @override
  Uint8List process(Uint8List imageBytes) {
    // 由 OcrPipeline 拦截走原生原子操作；直接调用时原样返回。
    return imageBytes;
  }
}

/// 自定义预处理器：宿主任意 Dart 图像处理逻辑。
class CustomTransformPreprocessor extends OcrPreprocessor {
  const CustomTransformPreprocessor(
    this.transform, {
    this.name = 'custom',
    this.scale = 1.0,
    this.recSource = true,
  });

  /// 变换函数：输入编码图字节，输出变体图编码字节。
  final Uint8List Function(Uint8List imageBytes) transform;

  @override
  final String name;

  @override
  final double scale;

  @override
  final bool recSource;

  @override
  Uint8List process(Uint8List imageBytes) => transform(imageBytes);
}

/// 快捷构造：与内置 upscale3x 分支等价的放大预处理器。
NativeTransformPreprocessor upscalePreprocessor({int times = 3}) {
  return NativeTransformPreprocessor(
    OcrTransformParams(scalePercent: times * 100),
    name: 'upscale${times}x',
    scale: times.toDouble(),
  );
}

/// 快捷构造：自适应二值化预处理器（只用于检测召回，不作识别源）。
NativeTransformPreprocessor binarizePreprocessor({
  int blockSize = 0,
  double c = 10.0,
}) {
  return NativeTransformPreprocessor(
    OcrTransformParams(
      binarizeMode: BinarizeMode.adaptiveGaussianInverted,
      adaptiveBlockSize: blockSize,
      adaptiveC: c,
      colorMode: ColorMode.gray3,
    ),
    name: 'adaptive_bin',
    recSource: false,
  );
}

/// 快捷构造：白字字幕提取预处理器（监控叠加字幕召回）。
NativeTransformPreprocessor whiteTextPreprocessor() {
  return NativeTransformPreprocessor(
    const OcrTransformParams(
      enhanceMode: EnhanceMode.whiteText,
      binarizeMode: BinarizeMode.adaptiveGaussianInverted,
      adaptiveBlockSize: 21,
      adaptiveC: 8.0,
      colorMode: ColorMode.gray3,
    ),
    name: 'white_text',
    recSource: false,
  );
}
