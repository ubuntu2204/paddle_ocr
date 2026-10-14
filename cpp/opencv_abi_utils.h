#ifndef OPENCV_ABI_UTILS_H_
#define OPENCV_ABI_UTILS_H_

#include <cstring>
#include <vector>

#include <opencv2/core.hpp>

namespace paddle_ocr {

// ---------------------------------------------------------------------------
// 把 std::vector<cv::Point> 复制进一个 (1, N, CV_32SC2) 的 cv::Mat。
//
// 为什么需要这一步：Windows 上插件以 /MDd 之类的 Debug CRT 配置编译宿主、
// 以 Release 配置编译 OpenCV 时，std::vector 的内存布局（_ITERATOR_DEBUGGING）
// 不一致。把点集先装进 cv::Mat（布局与 CRT 配置无关）再交给 InputArray，
// 就避免了 STL vector 跨 DLL 边界。
// ---------------------------------------------------------------------------
inline cv::Mat PointsToMat(const std::vector<cv::Point>& pts) {
  cv::Mat m(1, static_cast<int>(pts.size()), CV_32SC2);
  if (!pts.empty()) {
    std::memcpy(m.data, pts.data(), pts.size() * sizeof(cv::Point));
  }
  return m;
}

}  // namespace paddle_ocr

#endif  // OPENCV_ABI_UTILS_H_
