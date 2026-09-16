// 全屏截图接口
#pragma once
#include <cstdint>
#include <vector>
#include "Error.hpp"

namespace echonode::platform {

struct DirtyRect {
    int x = 0, y = 0, w = 0, h = 0;
};
struct MoveRect {
    int sx = 0, sy = 0, dx = 0, dy = 0, w = 0, h = 0;
};

class IScreenCapture {
public:
    virtual ~IScreenCapture() = default;

    virtual std::vector<uint8_t> captureBmp() = 0;

    virtual bool captureScaledRgb(std::vector<uint8_t>& out, int& w, int& h,
                                  double scale) = 0;


    virtual bool lastFrameMetadata(std::vector<DirtyRect>& /*dirty*/,
                                   std::vector<MoveRect>& /*moves*/) const {
        return false;
    }
};

} // namespace echonode::platform
