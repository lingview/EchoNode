// 全屏截图接口
#pragma once
#include <cstdint>
#include <vector>
#include "Error.hpp"

namespace echonode::platform {

class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;

    // 返回完整 BMP 文件字节流（含文件头）；无显示环境抛 PlatformError
    virtual std::vector<uint8_t> captureBmp() = 0;

    // 流式抓屏：抓取+降采样+BGR→RGB，输出顶到底 RGB24，w/h 为按 scale 算出的目标尺寸
    // 返回 false 表示画面无变化（out 仍是上次有效画面，首次成功前不得消费）；抛错=通道失效
    virtual bool captureScaledRgb(std::vector<uint8_t>& out, int& w, int& h,
                                  double scale) = 0;
};

} // namespace echonode::platform
