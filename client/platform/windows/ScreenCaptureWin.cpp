#include "IScreenCapture.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace echonode::platform {
namespace {

#pragma pack(push, 1)
struct BmpFileHeader {
    uint16_t type = 0x4D42; // 'BM'
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
    uint32_t compression = 0; // BI_RGB
    uint32_t imageSize = 0;
    int32_t xppm = 2835; // 72 DPI
    int32_t yppm = 2835;
    uint32_t colorsUsed = 0;
    uint32_t colorsImportant = 0;
};
#pragma pack(pop)

// 声明 DPI 感知，否则缩放屏下 GetSystemMetrics 返回逻辑尺寸与物理像素不符
void makeDpiAware() {
    static const bool done = [] {
        if (const auto fn = reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(
                GetProcAddress(GetModuleHandleW(L"user32.dll"),
                               "SetProcessDpiAwarenessContext"))) {
            if (fn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return true; // Per-Monitor V2
        }
        return SetProcessDPIAware() != 0;
    }();
    (void)done;
}

BITMAPINFO make32Info(int w, int h, bool topDown = false) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = topDown ? -h : h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return bi;
}

// MinGW 的 dxgi/d3d11 头只 extern 声明这些 IID（定义散在 libuuid 里，
// 且 MinGW 的 dxgi.h 压根没有 IDXGITexture2D），跨版本不稳定；
// 这里按头文件中 DEFINE_GUID 的字面值就地定义，去掉链接依赖。
// 值均取自 msys2/ucrt64/include 的 dxgi.h / dxgi1_2.h / d3d11.h。
constexpr GUID kDevIid{0x54ec77fa, 0x1377, 0x44e6,
                       {0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c}};    // IDXGIDevice
constexpr GUID kAdapter1Iid{0x29038f61, 0x3839, 0x4626,
                            {0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05}}; // IDXGIAdapter1
constexpr GUID kOutput1Iid{0x00cddea8, 0x939b, 0x4b83,
                           {0xa3, 0x40, 0xa6, 0x85, 0x22, 0x66, 0x66, 0xcc}};  // IDXGIOutput1
constexpr GUID kDupIid{0x191cfac3, 0xa341, 0x470d,
                       {0xb2, 0x6e, 0xa8, 0x64, 0xf4, 0x28, 0x31, 0x9c}};  // IDXGIOutputDuplication
constexpr GUID kTex2dIid{0x6f15aaf2, 0xd208, 0x4e89,
                         {0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c}};  // ID3D11Texture2D

// DXGI Desktop Duplication 取帧：GPU 合成结果直接读回，比 GDI 读回快一个量级，
// 且只在画面真的变化时才交付新帧
class DupCapture {
public:
    ~DupCapture() { release(); }

    // 失败原因写 reason（供上层打印一次）；成功后尺寸由 desc 给出
    bool init(std::string& reason) {
        // DuplicateOutput 要求设备带 BGRA_SUPPORT，否则回 E_ACCESSDENIED
        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        ID3D11Device* dev = nullptr;
        ID3D11DeviceContext* ctx = nullptr;
        if (const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                 flags, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                                 nullptr, &ctx);
            FAILED(hr)) {
            reason = "D3D11CreateDevice hr=" + hrText(hr);
            return false;
        }
        device_.Attach(dev);
        context_.Attach(ctx);

        IDXGIDevice* rawDev = nullptr;
        if (FAILED(device_->QueryInterface(kDevIid, reinterpret_cast<void**>(&rawDev)))) {
            reason = "设备无 IDXGIDevice 接口";
            return false;
        }
        ComPtr<IDXGIDevice> dxgiDev;
        dxgiDev.Attach(rawDev);

        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(dxgiDev->GetParent(kAdapter1Iid,
                                     reinterpret_cast<void**>(adapter.put())))) {
            reason = "取不到 IDXGIAdapter1（软件渲染驱动？）";
            return false;
        }

        // Duplication 是 per-output 的，多屏拼接的整虚拟屏给不了，那种场景回退 GDI
        RECT vs{};
        vs.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        vs.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        vs.right = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

        ComPtr<IDXGIOutput1> target;
        bool matched = false;
        for (UINT i = 0; !matched; ++i) {
            IDXGIOutput* out = nullptr;
            if (FAILED(adapter->EnumOutputs(i, &out))) break;
            ComPtr<IDXGIOutput> outPtr;
            outPtr.Attach(out);
            DXGI_OUTPUT_DESC d{};
            outPtr->GetDesc(&d);
            if (EqualRect(&d.DesktopCoordinates, &vs)) {
                if (SUCCEEDED(outPtr->QueryInterface(kOutput1Iid,
                                                     reinterpret_cast<void**>(target.put()))))
                    matched = true;
            }
        }
        if (!matched) {
            reason = "无输出与虚拟屏重合（多屏或分辨率切换中）";
            return false;
        }
        if (const HRESULT hr = target->DuplicateOutput(device_.get(), dup_.put());
            FAILED(hr)) {
            // 会话 0 / RDP 桌面 / 无显卡合成时会失败，属预期不可用
            reason = "DuplicateOutput hr=" + hrText(hr);
            if (hr == E_ACCESSDENIED)
                reason += "（RDP 会话、服务身份或安全桌面下不允许复制）";
            dup_.Reset();
            return false;
        }
        DXGI_OUTDUPL_DESC dd{};
        dup_->GetDesc(&dd);
        if (dd.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            dd.ModeDesc.Width == 0) {
            reason = "不支持的桌面格式 " +
                     std::to_string(static_cast<int>(dd.ModeDesc.Format));
            dup_.Reset();
            return false;
        }
        srcW_ = static_cast<int>(dd.ModeDesc.Width);
        srcH_ = static_cast<int>(dd.ModeDesc.Height);
        // 桌面纹理在显存里 CPU 不可读，建同尺寸 staging 纹理每帧 GPU→staging 后 Map
        if (!ensureStaging()) {
            reason = "staging 纹理创建失败";
            dup_.Reset();
            return false;
        }
        reason.clear();
        return true;
    }

    int srcWidth() const { return srcW_; }
    int srcHeight() const { return srcH_; }
    // 是否已交付过至少一帧（上层据此判断能不能拿「旧 out」当有效画面）
    bool delivered() const { return primed_; }

    bool metadataValid() const { return metadataValid_; }
    const std::vector<DirtyRect>& dirtyRects() const { return dirty_; }
    const std::vector<MoveRect>& moveRects() const { return moves_; }

    // 取一帧并降采样为顶到底 RGB24。返回 false=本次无新画面（out 保持上次内容，
    // w/h 仍报按 scale 算出的真实尺寸）。allowBlock 只给首次 acquire 一个等待窗口
    bool nextFrame(std::vector<uint8_t>& out, int& w, int& h, double scale,
                   bool allowBlock) {
        // 尺寸先定下来：false 路径也要能报正确 w/h，否则调用方拿到 0×0
        const int tw = std::max(1, static_cast<int>(srcW_ * scale));
        const int th = std::max(1, static_cast<int>(srcH_ * scale));
        w = tw;
        h = th;

        // 冷启动首帧无条件接受：静止桌面也要拿到画面，否则永远黑屏
        const bool first = !primed_ && allowBlock;
        DXGI_OUTDUPL_FRAME_INFO fi{};
        ComPtr<IDXGIResource> res;
        const HRESULT hr = dup_->AcquireNextFrame(first ? 500 : 0, &fi, res.put());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false; // 桌面没变
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_INVALID_CALL) {
            throw std::runtime_error("duplication 通道失效");
        }
        if (FAILED(hr)) throw std::runtime_error("AcquireNextFrame 失败");
        acquired_ = true;

        // 只有鼠标动了、画面内容没变：帧不变，回 false 但必须 ReleaseFrame
        const bool hasNewImage = fi.LastPresentTime.QuadPart != 0;
        struct FrameGuard {
            DupCapture* self;
            ~FrameGuard() { if (self->acquired_) { self->dup_->ReleaseFrame(); self->acquired_ = false; } }
        } guard{this};

        if (!hasNewImage && !first) return false;

        dirty_.clear();
        moves_.clear();
        metadataValid_ = false;
        if (fi.TotalMetadataBufferSize > 0) {
            std::vector<uint8_t> meta(fi.TotalMetadataBufferSize);
            UINT needed = 0;
            if (SUCCEEDED(dup_->GetFrameDirtyRects(fi.TotalMetadataBufferSize,
                                                   reinterpret_cast<RECT*>(meta.data()),
                                                   &needed))) {
                const RECT* r = reinterpret_cast<const RECT*>(meta.data());
                for (UINT i = 0; i < needed / sizeof(RECT); ++i) {
                    DirtyRect dr;
                    dr.x = static_cast<int>(r[i].left * scale);
                    dr.y = static_cast<int>(r[i].top * scale);
                    dr.w = static_cast<int>((r[i].right - r[i].left) * scale);
                    dr.h = static_cast<int>((r[i].bottom - r[i].top) * scale);
                    if (dr.w < 0) dr.w = 0;
                    if (dr.h < 0) dr.h = 0;
                    dirty_.push_back(dr);
                }
            }
            if (SUCCEEDED(dup_->GetFrameMoveRects(fi.TotalMetadataBufferSize,
                                                  reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(meta.data()),
                                                  &needed))) {
                const DXGI_OUTDUPL_MOVE_RECT* m =
                    reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(meta.data());
                for (UINT i = 0; i < needed / sizeof(DXGI_OUTDUPL_MOVE_RECT); ++i) {
                    MoveRect mv;
                    mv.sx = static_cast<int>(m[i].SourcePoint.x * scale);
                    mv.sy = static_cast<int>(m[i].SourcePoint.y * scale);
                    mv.dx = static_cast<int>(m[i].DestinationRect.left * scale);
                    mv.dy = static_cast<int>(m[i].DestinationRect.top * scale);
                    mv.w = static_cast<int>((m[i].DestinationRect.right - m[i].DestinationRect.left) * scale);
                    mv.h = static_cast<int>((m[i].DestinationRect.bottom - m[i].DestinationRect.top) * scale);
                    moves_.push_back(mv);
                }
            }
            metadataValid_ = true;
        }

        ComPtr<ID3D11Texture2D> tex;
        if (FAILED(res->QueryInterface(kTex2dIid, reinterpret_cast<void**>(tex.put()))))
            throw std::runtime_error("桌面帧无 ID3D11Texture2D 接口");

        if (tw != dstW_ || th != dstH_ || texW_ != srcW_) rebuildMaps(tw, th);

        // 显存帧 → staging（一次 DMA 拷贝）后才能 CPU 读
        if (!staging_ && !ensureStaging())
            throw std::runtime_error("staging 纹理创建失败");
        context_->CopyResource(staging_.get(), tex.get());

        D3D11_MAPPED_SUBRESOURCE mp{};
        if (FAILED(context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mp)))
            throw std::runtime_error("Map staging 失败");
        struct UnmapGuard {
            ID3D11DeviceContext* ctx;
            ~UnmapGuard() { ctx->Unmap(res_, 0); }
            ID3D11Texture2D* res_ = nullptr;
        } ug{context_.get(), nullptr};
        ug.res_ = staging_.get();

        // DXGI 帧是顶到底，行内 B8G8R8A8
        const auto* src = static_cast<const uint8_t*>(mp.pData);
        out.resize(static_cast<size_t>(tw) * th * 3);
        for (int y = 0; y < th; ++y) {
            const uint8_t* s = src + static_cast<size_t>(ymap_[y]) * mp.RowPitch;
            uint8_t* d = out.data() + static_cast<size_t>(y) * tw * 3;
            for (int x = 0; x < tw; ++x) {
                const uint8_t* p = s + xmap_[x];
                d[x * 3 + 0] = p[2];
                d[x * 3 + 1] = p[1];
                d[x * 3 + 2] = p[0];
            }
        }
        dstW_ = tw;
        dstH_ = th;
        primed_ = true; // 已交付过一帧，之后 false 才意味着「out 里已有有效画面」
        return true;
    }

    void release() {
        staging_.Reset();
        dup_.Reset();
        device_.Reset();
        context_.Reset();
        dstW_ = dstH_ = 0;
        primed_ = false;
    }

private:
    bool ensureStaging() {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = static_cast<UINT>(srcW_);
        sd.Height = static_cast<UINT>(srcH_);
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device_->CreateTexture2D(&sd, nullptr, staging_.put()));
    }
    // 目标像素 → 源像素偏移（一次算好，转换内层循环避免浮点乘除）
    void rebuildMaps(int tw, int th) {
        xmap_.resize(static_cast<size_t>(tw));
        for (int x = 0; x < tw; ++x)
            xmap_[x] = std::min(srcW_ - 1, static_cast<int>((x + 0.5) * srcW_ / tw)) * 4;
        ymap_.resize(static_cast<size_t>(th));
        for (int y = 0; y < th; ++y)
            ymap_[y] = std::min(srcH_ - 1, static_cast<int>((y + 0.5) * srcH_ / th));
        texW_ = srcW_;
    }

    static std::string hrText(HRESULT hr) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
        return buf;
    }

    template <class T>
    class ComPtr {
    public:
        ~ComPtr() { Reset(); }
        T** put() { Reset(); return &p_; }
        T* get() const { return p_; }
        T* operator->() const { return p_; }
        void Attach(T* p) { Reset(); p_ = p; }
        void Reset() {
            if (p_) p_->Release();
            p_ = nullptr;
        }
        explicit operator bool() const { return p_ != nullptr; }

    private:
        T* p_ = nullptr;
    };

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D> staging_;
    std::vector<int> xmap_, ymap_;
    int srcW_ = 0, srcH_ = 0;
    int dstW_ = 0, dstH_ = 0;
    int texW_ = 0; // maps 对应的源宽（尺寸变化判定）
    bool acquired_ = false;
    bool primed_ = false; // 是否已交付过至少一帧（区分冷启动与「桌面静止」）
    std::vector<DirtyRect> dirty_;
    std::vector<MoveRect> moves_;
    bool metadataValid_ = false;
};

class ScreenCaptureWin : public IScreenCapture {
public:
    ~ScreenCaptureWin() override { freeTarget(); }

    // 流式抓屏优先走 GPU，不可用时退回 GDI；agent 常驻，失败后定期重试（不永久放弃）
    bool captureScaledRgb(std::vector<uint8_t>& out, int& w, int& h,
                          double scale) override {
        makeDpiAware();
        lastFrameFromGpu_ = false;
        if (!gpuReady_) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= gpuRetryAt_) {
                gpuRetryAt_ = now + std::chrono::seconds(120);
                std::string reason;
                if (gpu_.init(reason)) {
                    gpuReady_ = true;
                    gpuBootstrapped_ = false; // 新通道重新引导一次首帧
                    std::cerr << "[desk] 抓屏后端=DXGI duplication "
                              << gpu_.srcWidth() << "x" << gpu_.srcHeight()
                              << std::endl;
                } else if (!gpuWarned_) {
                    // 只报一次，否则每 2 分钟刷同一行 stderr
                    gpuWarned_ = true;
                    std::cerr << "[desk] 抓屏后端=GDI（DXGI 不可用: " << reason
                              << "），之后每 2 分钟重试" << std::endl;
                }
            }
        }
        if (gpuReady_) {
            try {
                const bool got = gpu_.nextFrame(out, w, h, scale, !gpuBootstrapped_);
                // 首帧用 GDI 一次性引导（静止桌面时 DXGI 不主动交付当前画面）；
                // 但绝不能每帧回退 GDI：duplication 激活时 GDI 读回会拖到 267ms/帧
                if (got || gpu_.delivered() || gpuBootstrapped_) {
                    lastFrameFromGpu_ = got;
                    return got;
                }
                gdiScaledRgb(out, w, h, scale);
                gpuBootstrapped_ = true;
                return true;
            } catch (const std::exception& e) {
                // 通道失效（分辨率切换、会话断开）：接下来几帧走 GDI，5s 后重建 GPU 通道
                gpu_.release();
                gpuReady_ = false;
                gpuBootstrapped_ = false;
                gpuRetryAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                std::cerr << "[desk] DXGI 取帧中断，临时回退 GDI（5s 后重试）: "
                          << e.what() << std::endl;
            }
        }
        return gdiScaledRgb(out, w, h, scale);
    }

    bool lastFrameMetadata(std::vector<DirtyRect>& dirty,
                           std::vector<MoveRect>& moves) const override {
        if (!lastFrameFromGpu_ || !gpu_.metadataValid()) return false;
        dirty = gpu_.dirtyRects();
        moves = gpu_.moveRects();
        return true;
    }

    std::vector<uint8_t> captureBmp() override {
        makeDpiAware();

        // 虚拟屏幕 = 所有显示器拼接的整体，单屏时等价于主屏
        const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (w <= 0 || h <= 0) throw PlatformError("invalid screen metrics");

        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);

        BITMAPINFO bi = make32Info(w, h);
        std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
        if (!GetDIBits(mem, bmp, 0, h, pixels.data(), &bi, DIB_RGB_COLORS)) {
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            ReleaseDC(nullptr, screen);
            throw PlatformError("GetDIBits failed");
        }

        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);

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
    // GDI 回退路径：整帧读回 1440p 固定 ~33ms，缩放由平台侧完成，成本随目标分辨率变
    // 缩放用 COLORONCOLOR（HALFTONE 会触发 GDI 软件滤波慢路径）
    bool gdiScaledRgb(std::vector<uint8_t>& out, int& w, int& h, double scale) {
        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (vw <= 0 || vh <= 0) throw PlatformError("invalid screen metrics");
        const int tw = std::max(1, static_cast<int>(vw * scale));
        const int th = std::max(1, static_cast<int>(vh * scale));
        if (!ensureTarget(tw, th)) throw PlatformError("目标位图创建失败");

        HDC screen = GetDC(nullptr);
        bool ok = false;
        if (scale < 1.0) {
            SetStretchBltMode(mem_, COLORONCOLOR);
            ok = StretchBlt(mem_, 0, 0, tw, th, screen, vx, vy, vw, vh, SRCCOPY) != 0;
        } else {
            ok = BitBlt(mem_, 0, 0, tw, th, screen, vx, vy, SRCCOPY) != 0;
        }
        if (ok)
            ok = GetDIBits(mem_, dib_, 0, th, pix_.data(), &bi_, DIB_RGB_COLORS) != 0;
        ReleaseDC(nullptr, screen);
        if (!ok) throw PlatformError("GDI 抓屏失败");

        // GetDIBits 取到的是自底向上行序，转 RGB24 时顺带翻行
        out.resize(static_cast<size_t>(tw) * th * 3);
        for (int y = 0; y < th; ++y) {
            const uint8_t* s =
                pix_.data() + static_cast<size_t>(th - 1 - y) * tw * 4;
            uint8_t* d = out.data() + static_cast<size_t>(y) * tw * 3;
            for (int x = 0; x < tw; ++x) {
                d[x * 3 + 0] = s[x * 4 + 2];
                d[x * 3 + 1] = s[x * 4 + 1];
                d[x * 3 + 2] = s[x * 4 + 0];
            }
        }
        w = tw;
        h = th;
        return true;
    }

    // 目标位图缓存：只在缩放尺寸变化时重建（每帧新建/销毁 MB 级位图是浪费）
    bool ensureTarget(int tw, int th) {
        if (mem_ && dib_ && tw == tw_ && th == th_) return true;
        freeTarget();
        HDC screen = GetDC(nullptr);
        mem_ = CreateCompatibleDC(screen);
        dib_ = CreateCompatibleBitmap(screen, tw, th);
        ReleaseDC(nullptr, screen);
        if (!mem_ || !dib_) {
            freeTarget();
            return false;
        }
        // CreateCompatibleDC 自带的默认位图不先选回，DeleteDC 会失败
        orig_ = SelectObject(mem_, dib_);
        bi_ = make32Info(tw, th);
        pix_.resize(static_cast<size_t>(tw) * th * 4);
        tw_ = tw;
        th_ = th;
        return true;
    }

    void freeTarget() {
        if (mem_ && orig_) SelectObject(mem_, orig_);
        if (dib_) DeleteObject(dib_);
        if (mem_) DeleteDC(mem_);
        mem_ = nullptr;
        dib_ = nullptr;
        orig_ = nullptr;
        tw_ = th_ = 0;
    }

    DupCapture gpu_;
    bool gpuReady_ = false;
    bool lastFrameFromGpu_ = false;               // 上一帧是否来自 GPU（决定元数据是否可信）
    bool gpuBootstrapped_ = false;                // 首帧已用 GDI 引导过
    bool gpuWarned_ = false;                        // 回退日志只打一次
    std::chrono::steady_clock::time_point gpuRetryAt_{}; // 到点重试 GPU

    HDC mem_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ orig_ = nullptr;
    BITMAPINFO bi_{};
    std::vector<uint8_t> pix_;
    int tw_ = 0, th_ = 0;
};

} // namespace

std::unique_ptr<IScreenCapture> createScreenCapture() {
    return std::make_unique<ScreenCaptureWin>();
}

} // namespace echonode::platform
