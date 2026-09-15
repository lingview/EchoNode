#include "IScreenCapture.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h> // XDestroyImage 在这里，Xlib.h 没有
#include <algorithm>
#include <cstring>
#include <memory>

namespace echonode::platform {
namespace {

#pragma pack(push, 1)
struct BmpFileHeader {
    uint16_t type = 0x4D42;
    uint32_t size = 0;
    uint16_t reserved1 = 0;
    uint16_t reserved2 = 0;
    uint32_t offset = 0;
};

struct BmpInfoHeader {
    uint32_t size = 0;
    int32_t width = 0;
    int32_t height = 0;
    uint16_t planes = 1;
    uint16_t bpp = 32;
    uint32_t compression = 0;
    uint32_t imageSize = 0;
    int32_t xppm = 2835;
    int32_t yppm = 2835;
    uint32_t colorsUsed = 0;
    uint32_t colorsImportant = 0;
};
#pragma pack(pop)

// X11 的像素是 BGRX 排列，直接就是 BMP 的 32bpp 行格式，行序反转即可
std::vector<uint8_t> toBmpPixels(const XImage& img, int w, int h) {
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
    const int srcLine = img.bytes_per_line;
    for (int y = 0; y < h; ++y) {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(img.data) +
                             static_cast<size_t>(y) * srcLine;
        uint8_t* dst = out.data() + static_cast<size_t>(h - 1 - y) * w * 4;
        std::memcpy(dst, src, static_cast<size_t>(w) * 4);
    }
    return out;
}

class ScreenCaptureLinux : public IScreenCapture {
public:
    ~ScreenCaptureLinux() override {
        if (disp_) XCloseDisplay(disp_);
    }

    // 流式抓屏：复用 X 连接，XGetImage 后按 scale 降采样为顶到底 RGB24；恒返回 true
    // （X11 无 Desktop Duplication 式的“画面是否变了”通知，v1 不接 XDamage）
    bool captureScaledRgb(std::vector<uint8_t>& out, int& w, int& h,
                          double scale) override {
        if (!disp_) {
            disp_ = XOpenDisplay(nullptr);
            if (!disp_)
                throw PlatformError("XOpenDisplay failed: 无可用的 X 显示环境");
            root_ = DefaultRootWindow(disp_);
        }
        XWindowAttributes attr{};
        XGetWindowAttributes(disp_, root_, &attr);
        const int sw = attr.width;
        const int sh = attr.height;
        if (sw <= 0 || sh <= 0) throw PlatformError("invalid screen metrics");

        XImage* img =
            XGetImage(disp_, root_, 0, 0, sw, sh, AllPlanes, ZPixmap);
        if (!img) throw PlatformError("XGetImage failed");
        if (img->bits_per_pixel != 32) {
            const int bpp = img->bits_per_pixel;
            XDestroyImage(img);
            throw PlatformError("仅支持 32bpp 视觉，实际: " + std::to_string(bpp));
        }

        const int tw = std::max(1, static_cast<int>(sw * scale));
        const int th = std::max(1, static_cast<int>(sh * scale));
        out.resize(static_cast<size_t>(tw) * th * 3);
        const double fx = static_cast<double>(sw) / tw;
        const double fy = static_cast<double>(sh) / th;
        const int srcLine = img->bytes_per_line;
        for (int y = 0; y < th; ++y) {
            const int sy = std::min(sh - 1, static_cast<int>((y + 0.5) * fy));
            const uint8_t* src = reinterpret_cast<const uint8_t*>(img->data) +
                                 static_cast<size_t>(sy) * srcLine;
            uint8_t* d = out.data() + static_cast<size_t>(y) * tw * 3;
            for (int x = 0; x < tw; ++x, d += 3) {
                const int sxx =
                    std::min(sw - 1, static_cast<int>((x + 0.5) * fx)) * 4;
                d[0] = src[sxx + 2]; // BGR → RGB
                d[1] = src[sxx + 1];
                d[2] = src[sxx + 0];
            }
        }
        XDestroyImage(img);
        w = tw;
        h = th;
        return true;
    }

    std::vector<uint8_t> captureBmp() override {
        Display* disp = XOpenDisplay(nullptr);
        if (!disp) throw PlatformError("XOpenDisplay failed: 无可用的 X 显示环境");

        Window root = DefaultRootWindow(disp);
        XWindowAttributes attr{};
        XGetWindowAttributes(disp, root, &attr);
        const int w = attr.width;
        const int h = attr.height;

        XImage* img = XGetImage(disp, root, 0, 0, w, h, AllPlanes, ZPixmap);
        if (!img) {
            XCloseDisplay(disp);
            throw PlatformError("XGetImage failed");
        }
        if (img->bits_per_pixel != 32) {
            XDestroyImage(img);
            XCloseDisplay(disp);
            throw PlatformError("仅支持 32bpp 视觉，实际: " + std::to_string(img->bits_per_pixel));
        }

        std::vector<uint8_t> pixels = toBmpPixels(*img, w, h);
        XDestroyImage(img);
        XCloseDisplay(disp);

        BmpFileHeader fh;
        BmpInfoHeader ih;
        ih.size = sizeof(BmpInfoHeader);
        ih.width = w;
        ih.height = h;
        ih.imageSize = static_cast<uint32_t>(pixels.size());
        fh.offset = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
        fh.size = fh.offset + ih.imageSize;

        std::vector<uint8_t> out(fh.size);
        std::memcpy(out.data(), &fh, sizeof(fh));
        std::memcpy(out.data() + sizeof(fh), &ih, sizeof(ih));
        std::memcpy(out.data() + fh.offset, pixels.data(), pixels.size());
        return out;
    }

private:
    Display* disp_ = nullptr; // 流式抓屏复用连接，避免每帧一次 X 往返
    Window root_ = 0;
};

} // namespace

std::unique_ptr<IScreenCapture> createScreenCapture() {
    return std::make_unique<ScreenCaptureLinux>();
}

} // namespace echonode::platform
