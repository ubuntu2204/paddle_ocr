import 'dart:developer' as developer;
import 'package:flutter/services.dart';

import 'paddle_ocr_platform_interface.dart';
import 'src/ocr_result.dart';

/// MethodChannel implementation of [PaddleOcrPlatform].
class MethodChannelPaddleOcr extends PaddleOcrPlatform {
  final MethodChannel _channel = const MethodChannel('paddle_ocr');

  /// Log a debug message.
  void _log(String message, {Object? error, StackTrace? stackTrace}) {
    developer.log(message, name: 'PaddleOcr', error: error, stackTrace: stackTrace);
    // Also print to console for easy visibility
    // ignore: avoid_print
    print('[PaddleOcr] $message');
    if (error != null) {
      // ignore: avoid_print
      print('[PaddleOcr]   error: $error');
    }
  }

  @override
  Future<bool> initialize({
    required String detModelPath,
    required String recModelPath,
    required String dictPath,
  }) async {
    _log('initialize: detModelPath="$detModelPath"');
    _log('initialize: recModelPath="$recModelPath"');
    _log('initialize: dictPath="$dictPath"');

    try {
      final result = await _channel.invokeMethod<bool>('initialize', {
        'detModelPath': detModelPath,
        'recModelPath': recModelPath,
        'dictPath': dictPath,
      });
      _log('initialize: native returned result=$result');
      return result ?? false;
    } on PlatformException catch (e) {
      _log('initialize: PlatformException code=${e.code} message=${e.message} '
          'details=${e.details}');
      rethrow;
    } catch (e, st) {
      _log('initialize: unexpected error', error: e, stackTrace: st);
      rethrow;
    }
  }

  @override
  Future<List<OcrResult>> recognizeImage(String imagePath) async {
    _log('recognizeImage: imagePath="$imagePath"');
    _log('recognizeImage: path codeUnits=${imagePath.codeUnits}');

    try {
      final result = await _channel.invokeMethod<List<dynamic>>('recognizeImage', {
        'imagePath': imagePath,
      });
      _log('recognizeImage: native returned ${result?.length ?? 0} items');
      if (result == null) return [];
      return _parseResults(result);
    } on FormatException catch (e) {
      _log('recognizeImage: FormatException! offset=${e.offset} message=${e.message}');
      _log('recognizeImage: This usually means native code returned invalid UTF-8 bytes.');
      _log('recognizeImage: Check the native stderr log for hex dumps of the data.');
      rethrow;
    } on PlatformException catch (e) {
      _log('recognizeImage: PlatformException code=${e.code} message=${e.message} '
          'details=${e.details}');
      rethrow;
    } catch (e, st) {
      _log('recognizeImage: unexpected error', error: e, stackTrace: st);
      rethrow;
    }
  }

  @override
  Future<List<OcrResult>> recognizeImageBytes(Uint8List imageBytes) async {
    _log('recognizeImageBytes: ${imageBytes.length} bytes');

    try {
      final result =
          await _channel.invokeMethod<List<dynamic>>('recognizeImageBytes', {
        'imageBytes': imageBytes,
      });
      _log('recognizeImageBytes: native returned ${result?.length ?? 0} items');
      if (result == null) return [];
      return _parseResults(result);
    } on FormatException catch (e) {
      _log('recognizeImageBytes: FormatException! offset=${e.offset} message=${e.message}');
      _log('recognizeImageBytes: This usually means native code returned invalid UTF-8 bytes.');
      rethrow;
    } on PlatformException catch (e) {
      _log('recognizeImageBytes: PlatformException code=${e.code} message=${e.message}');
      rethrow;
    } catch (e, st) {
      _log('recognizeImageBytes: unexpected error', error: e, stackTrace: st);
      rethrow;
    }
  }

  /// Parse the raw result list from native, with detailed error reporting.
  List<OcrResult> _parseResults(List<dynamic> rawList) {
    final results = <OcrResult>[];
    for (int i = 0; i < rawList.length; i++) {
      final item = rawList[i];
      _log('_parseResults: item[$i] type=${item.runtimeType}');
      try {
        final map = item as Map<dynamic, dynamic>;
        // Log the raw map keys and text for debugging
        final keys = map.keys.map((k) => k.toString()).toList();
        _log('_parseResults: item[$i] keys=$keys');

        final text = map['text'];
        _log('_parseResults: item[$i] text type=${text.runtimeType} value="$text"');

        if (text is String) {
          _log('_parseResults: item[$i] text codeUnits=${text.codeUnits}');
        }

        final result = OcrResult.fromMap(map);
        _log('_parseResults: item[$i] parsed OK: $result');
        results.add(result);
      } on FormatException catch (e) {
        _log('_parseResults: FormatException at item[$i]! offset=${e.offset} '
            'message=${e.message}');
        // Log the raw item data for debugging
        if (item is Map) {
          final text = item['text'];
          if (text is String) {
            _log('_parseResults: item[$i] text had invalid encoding. '
                'codeUnits=${text.codeUnits}');
          }
        }
        rethrow;
      } catch (e, st) {
        _log('_parseResults: error parsing item[$i]', error: e, stackTrace: st);
        rethrow;
      }
    }
    return results;
  }

  @override
  Future<String?> pickImage() async {
    _log('pickImage: calling native picker');
    try {
      final path = await _channel.invokeMethod<String>('pickImage');
      _log('pickImage: returned path="$path"');
      if (path != null && path.isNotEmpty) {
        _log('pickImage: path codeUnits=${path.codeUnits}');
      }
      return path;
    } on FormatException catch (e) {
      _log('pickImage: FormatException! offset=${e.offset} message=${e.message}');
      _log('pickImage: This means the file path contains non-UTF-8 bytes.');
      _log('pickImage: The native picker should use GetOpenFileNameW and convert to UTF-8.');
      rethrow;
    } on PlatformException catch (e) {
      _log('pickImage: PlatformException code=${e.code} message=${e.message}');
      rethrow;
    } catch (e, st) {
      _log('pickImage: unexpected error', error: e, stackTrace: st);
      rethrow;
    }
  }

  @override
  Future<String?> getPlatformVersion() async {
    _log('getPlatformVersion: calling native');
    try {
      final version = await _channel.invokeMethod<String>('getPlatformVersion');
      _log('getPlatformVersion: returned "$version"');
      return version;
    } catch (e, st) {
      _log('getPlatformVersion: error', error: e, stackTrace: st);
      rethrow;
    }
  }
}
