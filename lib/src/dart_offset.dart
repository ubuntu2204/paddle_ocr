import 'dart:math' as math;

/// 纯 Dart（Dart VM / `dart compile exe`）环境下的 [Offset] 替身。
///
/// Flutter 构建时通过条件导入使用 `dart:ui` 的原生 `Offset`；本文件只在
/// 没有 `dart:ui` 的环境（后端服务、命令行工具）里参与编译。API 保持与
/// `dart:ui.Offset` 的最小兼容，调用方无需区分两种实现。
class Offset {
  const Offset(this.dx, this.dy);

  /// 零偏移，与 `dart:ui.Offset.zero` 一致。
  static const Offset zero = Offset(0, 0);

  final double dx;
  final double dy;

  Offset operator +(Offset other) => Offset(dx + other.dx, dy + other.dy);

  Offset operator -(Offset other) => Offset(dx - other.dx, dy - other.dy);

  Offset operator *(double operand) => Offset(dx * operand, dy * operand);

  Offset operator /(double operand) => Offset(dx / operand, dy / operand);

  Offset negate() => Offset(-dx, -dy);

  Offset translate(double offsetDx, double offsetDy) =>
      Offset(dx + offsetDx, dy + offsetDy);

  double get distanceSquared => dx * dx + dy * dy;

  double get distance => math.sqrt(distanceSquared);

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      (other is Offset && other.dx == dx && other.dy == dy);

  @override
  int get hashCode => Object.hash(dx, dy);

  @override
  String toString() => 'Offset($dx, $dy)';
}
