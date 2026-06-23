import 'package:flutter_test/flutter_test.dart';

import 'package:paddle_ocr_example/main.dart';

void main() {
  testWidgets('App launches without errors', (WidgetTester tester) async {
    await tester.pumpWidget(const OcrExampleApp());
    expect(find.text('PP-OCRv6 Offline OCR Demo'), findsOneWidget);
  });
}
