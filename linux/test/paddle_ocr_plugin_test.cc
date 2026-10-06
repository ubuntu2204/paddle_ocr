// Linux-specific C++ plugin tests.
//
// These are the counterpart of windows/test/paddle_ocr_plugin_test.cpp.
// The main plugin class on Linux is a C entry point (rather than a C++
// class with private HandleMethodCall), so we cannot invoke it directly
// without a real FlPluginRegistrar. What we CAN test at this layer is:
//   1. The plugin's exported registration symbol is present.
//   2. The shared C++ header used by the Linux build compiles cleanly
//      on this platform (basic header sanity).
//
// All meaningful behavior lives in cpp/test/cpp_shared_tests.cpp which
// is linked alongside this file into the same test runner.
#include <gtest/gtest.h>

#include "pp_ocr_ffi.h"

// The exported C entry point from the Linux plugin. Declared here to
// avoid pulling in the full flutter_linux headers just to assert that
// the symbol is linkable.
extern "C" void paddle_ocr_plugin_register_with_registrar(void* registrar);

namespace paddle_ocr {
namespace test {

TEST(LinuxPluginSymbol, RegisterEntrypointIsLinkable) {
  // We do not have a live FlPluginRegistrar in a unit test, so we cannot
  // actually call it. Just verify the function pointer is non-null after
  // taking its address; the linker would have failed earlier otherwise.
  auto fn = &paddle_ocr_plugin_register_with_registrar;
  EXPECT_NE(fn, nullptr);
}

TEST(LinuxPluginSymbol, FfiExportIsLinkable) {
  // The plugin .so exports both the C API (for dart:ffi) and the
  // registrar entry point. Verify the FFI side is linkable from the
  // same shared library.
  auto fn = &pp_ocr_create;
  EXPECT_NE(fn, nullptr);
}

}  // namespace test
}  // namespace paddle_ocr
