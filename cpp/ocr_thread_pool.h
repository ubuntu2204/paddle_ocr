#ifndef PADDLE_OCR_THREAD_POOL_H_
#define PADDLE_OCR_THREAD_POOL_H_

#include <algorithm>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

// 管线并行化开关。置 0 可完全回退到串行实现（例如某些交叉工具链的
// std::thread 链接有问题时）。默认开启。
#ifndef PP_OCR_PARALLEL_PIPELINE
#define PP_OCR_PARALLEL_PIPELINE 1
#endif

namespace paddle_ocr {

// ===========================================================================
// 管线并行度上限（引擎与 FFI 批量接口共用）
//
// 分支检测是整图推理（重），区域识别是小裁剪推理（轻，且共享识别会话的
// 线程池）。留上限避免线程创建开销吃掉收益。
// ===========================================================================
constexpr size_t kMaxBranchThreads = 2;
constexpr size_t kMaxRegionThreads = 12;

// ===========================================================================
// 极简并行 for：把 [0, count) 下标分给一组临时线程执行 fn(i)。
//
// 适用约束（调用方负责保证）：
//   * fn 之间无共享可变状态（只写各自下标对应的输出槽位）；
//   * fn 内部调用的 ONNX Runtime `Session::Run` 可重入（官方保证）；
//   * fn 不抛异常穿越线程边界（引擎内部已 catch）。
//
// 设计取舍：用「每次调用临时起线程」而不是持久线程池 —— 管线每次识别
// 只并行两三个阶段，线程创建开销（几十微秒）相对秒级推理可忽略；
// 换来的是无静态析构顺序问题、无线程残留（Flutter 热退出也安全）。
// ===========================================================================
inline void ParallelFor(size_t count, const std::function<void(size_t)>& fn,
                        size_t max_threads) {
#if PP_OCR_PARALLEL_PIPELINE
  if (count <= 1 || max_threads <= 1) {
    for (size_t i = 0; i < count; ++i) fn(i);
    return;
  }
  unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) hw = 2;  // 探测失败时保守双线程
  size_t nthreads = std::min({count, max_threads, static_cast<size_t>(hw)});
  if (nthreads <= 1) {
    for (size_t i = 0; i < count; ++i) fn(i);
    return;
  }

  std::atomic<size_t> next{0};
  auto worker = [&]() {
    for (;;) {
      size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= count) break;
      fn(i);
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(nthreads - 1);
  for (size_t t = 1; t < nthreads; ++t) threads.emplace_back(worker);
  worker();  // 调用线程也参与，少一个空闲线程
  for (auto& th : threads) th.join();
#else
  (void)max_threads;
  for (size_t i = 0; i < count; ++i) fn(i);
#endif
}

// 会话（ORT）推理线程数：留一半核给并行阶段的其他线程，封顶 8。
// 实测（24 核）：封顶 8 与 12 墙钟相同，8 的总 CPU 更省；识别器
// （小张量 + 高并发）试过 1/2 线程，聚合吞吐反而下降（模型足够大，
// 摊得开），因此检测/识别共用这一档。
// 单次 Run 的并发 Run 调用共享同一会话的线程池，不会超订。
inline int SessionThreadCount() {
  unsigned hw = std::thread::hardware_concurrency();
  if (hw == 0) return 4;
  unsigned t = std::max(2u, std::min(8u, hw / 2));
  return static_cast<int>(t);
}

}  // namespace paddle_ocr

#endif  // PADDLE_OCR_THREAD_POOL_H_
