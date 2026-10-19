#ifndef PADDLE_OCR_TIMING_H_
#define PADDLE_OCR_TIMING_H_

// 分阶段墙钟计时（诊断用，默认零开销）。
//
// 用法：
//   PaddleOcrTimer t("detect.infer");
//   ... 干活的代码 ...
//   t.Stop();  // 析构也会自动 Stop，重复调用安全
//
// 输出走 OCR_LOG（stderr，PADDLE_OCR_DEBUG 编译开关控制），统计值可通过
// PaddleOcrTimer::Report() 一次性汇总打印，便于基准脚本聚合。
// 全部实现都在头里（inline + 函数内静态量），不引入新的编译单元。

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include "debug_utils.h"

namespace paddle_ocr {

// 是否启用计时：编译期默认关；运行时读环境变量 PP_OCR_TIMING=1 打开。
// 结果只进 stderr 日志，不改变任何识别行为。
inline bool TimingEnabled() {
#ifdef PP_OCR_TIMING
  return PP_OCR_TIMING != 0;
#else
  static const bool enabled = [] {
    const char* v = std::getenv("PP_OCR_TIMING");
    return v != nullptr && v[0] != '\0' && v[0] != '0';
  }();
  return enabled;
#endif
}

// 单段计时器。可嵌套（不同 tag），累加到全局统计表。
class PaddleOcrTimer {
 public:
  explicit PaddleOcrTimer(const char* tag, bool enabled = true)
      : tag_(tag ? tag : "?"), enabled_(enabled && TimingEnabled()) {
    if (enabled_) start_ = Clock::now();
  }
  ~PaddleOcrTimer() { Stop(); }

  PaddleOcrTimer(const PaddleOcrTimer&) = delete;
  PaddleOcrTimer& operator=(const PaddleOcrTimer&) = delete;

  void Stop() {
    if (enabled_ && !stopped_) {
      const double ms =
          std::chrono::duration<double, std::milli>(Clock::now() - start_)
              .count();
      Accumulate(tag_, ms);
      OCR_LOG("TIMING %s = %.3f ms", tag_, ms);
      stopped_ = true;
    }
  }

  struct Entry {
    std::string tag;
    int64_t count = 0;
    double total_ms = 0.0;
    double max_ms = 0.0;
  };

  // 全局统计累加（线程安全）。同名 tag 合并次数与总时长。
  static void Accumulate(const std::string& tag, double ms) {
    if (!TimingEnabled()) return;
    std::lock_guard<std::mutex> lock(RegistryMutex());
    for (auto& e : Registry()) {
      if (e.tag == tag) {
        e.count++;
        e.total_ms += ms;
        e.max_ms = std::max(e.max_ms, ms);
        return;
      }
    }
    Entry e;
    e.tag = tag;
    e.count = 1;
    e.total_ms = ms;
    e.max_ms = ms;
    Registry().push_back(std::move(e));
  }

  // 汇总表快照（按总时长降序）。
  static std::vector<Entry> Snapshot() {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    std::vector<Entry> out = Registry();
    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) {
                return a.total_ms > b.total_ms;
              });
    return out;
  }

  // 清空统计（基准每轮开始前调）。
  static void Reset() {
    std::lock_guard<std::mutex> lock(RegistryMutex());
    Registry().clear();
  }

  // 打印汇总到 stderr。
  static void Report(const char* title) {
    if (!TimingEnabled()) return;
    auto snap = Snapshot();
    fprintf(stderr, "===== TIMING SUMMARY: %s =====\n", title ? title : "");
    for (const auto& e : snap) {
      fprintf(stderr,
              "  %-40s n=%-5lld total=%9.2f ms  avg=%8.3f ms  max=%8.3f ms\n",
              e.tag.c_str(), static_cast<long long>(e.count), e.total_ms,
              e.count > 0 ? e.total_ms / static_cast<double>(e.count) : 0.0,
              e.max_ms);
    }
    fflush(stderr);
  }

 private:
  using Clock = std::chrono::steady_clock;
  static std::mutex& RegistryMutex() {
    static std::mutex m;
    return m;
  }
  static std::vector<Entry>& Registry() {
    static std::vector<Entry> r;
    return r;
  }

  const char* tag_;
  bool enabled_;
  bool stopped_ = false;
  Clock::time_point start_{};
};

}  // namespace paddle_ocr

#endif  // PADDLE_OCR_TIMING_H_
