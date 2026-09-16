#include "RemoteDeskExecutor.hpp"
#include "platform/PlatformFactory.hpp"
#include "util/Uuid.hpp"

#include <nlohmann/json.hpp>

// stb 实现由 StbImageWrite.cpp 唯一提供，这里仅取声明
#include "stb/stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>  // timeBeginPeriod：见下方定时器粒度说明（需链接 winmm）
#else
#include <X11/Xlib.h>
#endif

namespace echonode::executor {

using protocol::Task;
using protocol::TaskResult;

namespace {

constexpr int kTile = 64; // tile 边长（降采样后像素）

// 当前光标相对虚拟屏的像素坐标；抓屏不含光标，光标走 0x04/0x05 流
bool getCursorLogical(int& x, int& y, int& screenW, int& screenH) {
#ifdef _WIN32
    POINT pt{};
    if (!GetCursorPos(&pt)) return false;
    x = pt.x - GetSystemMetrics(SM_XVIRTUALSCREEN);
    y = pt.y - GetSystemMetrics(SM_YVIRTUALSCREEN);
    screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return screenW > 0 && screenH > 0;
#else
    static Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) return false;
    Window root, child;
    int rx = 0, ry = 0, wx = 0, wy = 0;
    unsigned int mask = 0;
    if (!XQueryPointer(dpy, DefaultRootWindow(dpy), &root, &child,
                       &rx, &ry, &wx, &wy, &mask))
        return false;
    screenW = DisplayWidth(dpy, DefaultScreen(dpy));
    screenH = DisplayHeight(dpy, DefaultScreen(dpy));
    x = rx;
    y = ry;
    return screenW > 0 && screenH > 0;
#endif
}

#ifdef _WIN32

// 当前光标的形状位图（RGBA）+ 热点；旧式 HCURSOR 的透明由 AND 掩码表达
bool getCursorImage(std::vector<uint8_t>& rgba, int& w, int& h,
                    int& hotX, int& hotY, void** shapeId) {
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) return false;
    ICONINFO ii{};
    if (!GetIconInfo(ci.hCursor, &ii)) return false;
    *shapeId = ci.hCursor;
    hotX = ii.xHotspot;
    hotY = ii.yHotspot;

    BITMAP bmMask{};
    GetObject(ii.hbmMask, sizeof(bmMask), &bmMask);
    w = bmMask.bmWidth;
    h = ii.hbmColor ? bmMask.bmHeight : bmMask.bmHeight / 2;
    rgba.resize(static_cast<size_t>(w) * h * 4);

    HDC dc = GetDC(nullptr);
    auto extractBits = [&](HBITMAP hbmp, int startScan, int rows,
                           std::vector<uint8_t>& out) {
        out.assign(static_cast<size_t>(w) * rows * 4, 0);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = rows;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        GetDIBits(dc, hbmp, startScan, rows, out.data(), &bi, DIB_RGB_COLORS);
    };

    if (ii.hbmColor) {
        std::vector<uint8_t> color;
        extractBits(ii.hbmColor, 0, h, color);
        BITMAP bmColor{};
        GetObject(ii.hbmColor, sizeof(bmColor), &bmColor);
        const bool is32 = bmColor.bmBitsPixel == 32;
        std::vector<uint8_t> andBits;
        if (!is32) extractBits(ii.hbmMask, h, h, andBits); // AND 半区在掩码顶部
        for (int y = 0; y < h; ++y) {
            const int srcY = h - 1 - y;
            for (int x = 0; x < w; ++x) {
                const uint8_t* s =
                    &color[(static_cast<size_t>(srcY) * w + x) * 4];
                uint8_t* d = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                if (is32) {
                    const int a = s[3];
                    d[3] = static_cast<uint8_t>(a);
                    if (a > 0) {
                        d[0] = static_cast<uint8_t>(std::min(255, s[2] * 255 / a)); // R
                        d[1] = static_cast<uint8_t>(std::min(255, s[1] * 255 / a)); // G
                        d[2] = static_cast<uint8_t>(std::min(255, s[0] * 255 / a)); // B
                    } else {
                        d[0] = d[1] = d[2] = 0;
                    }
                } else {
                    const uint8_t* a =
                        &andBits[(static_cast<size_t>(srcY) * w + x) * 4];
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                    const bool andBit = a[0] != 0;
                    const bool black =
                        s[0] == 0 && s[1] == 0 && s[2] == 0;
                    d[3] = (andBit && black) ? 0 : 255;
                }
            }
        }
    } else {
        // 单色光标：掩码上半 AND + 下半 XOR
        std::vector<uint8_t> maskBits;
        extractBits(ii.hbmMask, 0, bmMask.bmHeight, maskBits);
        for (int y = 0; y < h; ++y) {
            const int srcAndY = bmMask.bmHeight - 1 - y;
            const int srcXorY = h - 1 - y;
            for (int x = 0; x < w; ++x) {
                const uint8_t* a =
                    &maskBits[(static_cast<size_t>(srcAndY) * w + x) * 4];
                const uint8_t* x8 =
                    &maskBits[(static_cast<size_t>(srcXorY) * w + x) * 4];
                uint8_t* d = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                const bool andBit = a[0] != 0;
                const bool xorBit = x8[0] != 0;
                if (andBit && !xorBit) { d[0] = d[1] = d[2] = d[3] = 0; }
                else if (!andBit && !xorBit) { d[0] = d[1] = d[2] = 0; d[3] = 255; }
                else if (!andBit && xorBit) { d[0] = d[1] = d[2] = 255; d[3] = 255; }
                else { d[0] = d[1] = d[2] = 255; d[3] = 255; }
            }
        }
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    ReleaseDC(nullptr, dc);
    return true;
}

#else

// Linux：X11 光标位图提取依赖 XFixes，v1 用内置箭头占位
bool getCursorImage(std::vector<uint8_t>& rgba, int& w, int& h,
                    int& hotX, int& hotY, void** shapeId) {
    static const char* shape[] = {
        "X.......", "XX......", "XXX.....", "XXXX....", "XXXXX...",
        "XXXXXX..", "XXXXXXX.", "XXXX....", "XX.XX...", "X..XX..."};
    w = 8;
    h = 10;
    hotX = 0;
    hotY = 0;
    *shapeId = (void*)1;
    rgba.assign(static_cast<size_t>(w) * h * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* d = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            if (shape[y][x] == 'X') {
                d[0] = d[1] = d[2] = 255;
                d[3] = 255;
            } else {
                d[3] = 0;
            }
        }
    return true;
}

#endif

// stb 内存编码回调：追加到 vector
void stbAppend(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(ctx);
    const auto* p = static_cast<const uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

// 把一个 tile 从整帧裁成紧凑缓冲再交 stb 编码；各 tile 输出独立，可并行
void encodeTile(const std::vector<uint8_t>& rgb, int frameW, int x0, int y0,
                int tw, int th, int quality, std::vector<uint8_t>& jpeg) {
    std::vector<uint8_t> buf(static_cast<size_t>(tw) * th * 3);
    for (int y = 0; y < th; ++y) {
        std::memcpy(&buf[static_cast<size_t>(y) * tw * 3],
                    &rgb[static_cast<size_t>(y0 + y) * frameW * 3 +
                         static_cast<size_t>(x0) * 3],
                    static_cast<size_t>(tw) * 3);
    }
    jpeg.clear();
    stbi_write_jpg_to_func(stbAppend, &jpeg, tw, th, 3, buf.data(), quality);
}

// 按原子游标瓜分 tile 多线程编码（tile 间无依赖，stb JPEG 无可变全局状态）；
// 脏 tile 不足 8 个时不起线程，建线程本身的开销不划算
void encodeTilesParallel(const std::vector<uint8_t>& rgb, int frameW, int frameH,
                         const std::vector<std::pair<int, int>>& tiles,
                         int quality, int tileSide,
                         std::vector<std::vector<uint8_t>>& out) {
    out.assign(tiles.size(), {});
    std::atomic<size_t> cursor{0};
    const auto drain = [&] {
        for (;;) {
            const size_t i = cursor.fetch_add(1, std::memory_order_relaxed);
            if (i >= tiles.size()) return;
            const int x0 = tiles[i].first * tileSide;
            const int y0 = tiles[i].second * tileSide;
            encodeTile(rgb, frameW, x0, y0, std::min(tileSide, frameW - x0),
                       std::min(tileSide, frameH - y0), quality, out[i]);
        }
    };
    if (tiles.size() < 8) {
        drain();
        return;
    }
    const unsigned cores = std::thread::hardware_concurrency();
    const size_t lanes =
        std::min(tiles.size(), std::min<size_t>(cores ? cores : 2, 6));
    std::vector<std::thread> helpers;
    helpers.reserve(lanes - 1);
    for (size_t i = 1; i < lanes; ++i) helpers.emplace_back(drain);
    drain();
    for (auto& t : helpers) t.join();
}

} // namespace

std::vector<std::string> RemoteDeskExecutor::actions() const {
    return {"remote_start", "remote_stop", "remote_input"};
}

RemoteDeskExecutor::~RemoteDeskExecutor() { stopLoop(); }

TaskResult RemoteDeskExecutor::execute(Task task) {
    if (task.action == "remote_start") {
        const int fps = std::clamp(task.payload.value("fps", 24), 1, 60);
        const int quality =
            std::clamp(task.payload.value("quality", 60), 20, 95);
        scale_ = std::clamp(task.payload.value("scale", 0.5), 0.25, 1.0);
        stopLoop();
        streamTaskId_ = task.taskId;
        fps_ = fps;
        quality_ = quality;

        int w = 0, h = 0;
        try {
            auto capture = echonode::platform::createScreenCapture();
            std::vector<uint8_t> rgb;
            // 首帧多试几次；拿不到像素就报错，绝不把 0×0 报给前端（会把 canvas 尺寸设成 0）
            for (int i = 0; i < 8 && rgb.empty(); ++i) {
                capture->captureScaledRgb(rgb, w, h, scale_);
                if (rgb.empty())
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (rgb.empty() || w <= 0 || h <= 0)
                return {task.taskId, false, {}, "screen capture returned no pixels"};
        } catch (const std::exception&) {
            return {task.taskId, false, {}, "screen capture unavailable"};
        }

        startLoop(task.taskId, fps, quality);
        return {task.taskId, true,
                nlohmann::json{{"w", w}, {"h", h}}.dump(), {}};
    }
    if (task.action == "remote_stop") {
        stopLoop();
        return {task.taskId, true, "stopped", {}};
    }
    if (task.action == "remote_input") {
        if (!injector_) {
            return {task.taskId, false, {}, "injector unavailable"};
        }
        for (const auto& ev : task.payload.value("events",
                                                  nlohmann::json::array())) {
            const std::string kind = ev.value("kind", "");
            if (kind == "move") {
                injector_->mouseMove(ev.value("nx", 0.0), ev.value("ny", 0.0));
            } else if (kind == "button") {
                injector_->mouseButton(ev.value("button", 0),
                                       ev.value("down", true));
            } else if (kind == "wheel") {
                injector_->mouseWheel(ev.value("dy", 0));
            } else if (kind == "key") {
                injector_->key(ev.value("code", ""), ev.value("down", true));
            }
        }
        return {task.taskId, true, "ok", {}};
    }
    return {task.taskId, false, {}, "unsupported action"};
}

void RemoteDeskExecutor::startLoop(const std::string& taskId, int fps,
                                   int quality) {
    running_ = true;
    worker_ = std::thread([this, taskId, fps, quality] {
#ifdef _WIN32
        // 会话期间把定时器粒度提到 1ms（默认 15.6ms 会压住帧率），退出时还原
        timeBeginPeriod(1);
#endif
        // 用微秒算间隔，避免毫秒取整丢帧率预算
        const auto interval =
            std::chrono::microseconds(1000000 / std::max(1, fps));
        uint32_t seq = 0;
        double lastCx = -1, lastCy = -1;
        void* lastShape = nullptr;

        try {
            auto capture = echonode::platform::createScreenCapture();

            // tile 网格（降采样后分辨率确定后初始化）
            int gridW = 0, gridH = 0, frameW = 0, frameH = 0;
            std::vector<uint64_t> tileSums;   // 每 tile 字节和（脏检测）
            // 待发送队列：脏 tile 全量入队、每 tick 按预算取，裁掉的不重扫（否则静止后残缺永久补不回）
            std::vector<std::pair<int, int>> pending;
            std::vector<uint8_t> queued; // 与 tileSums 等长：是否已在队列中（去重）
            size_t pendingHead = 0;

            // 组装并发送 0x06 增量帧：头 + tileCols/tileRows/tileW/tileH/count + tiles
            auto sendDelta = [&](int cols, int rows, int tw, int th,
                                 const std::vector<std::pair<int, int>>& tiles,
                                 const std::vector<std::vector<uint8_t>>& jpegs) {
                if (!binarySender_) return;
                // 布局：21B 头 + 10B 网格规格 + count×8B 元数据 + JPEG 数据连排
                size_t dataSize = 0;
                for (const auto& j : jpegs) dataSize += j.size();
                std::vector<uint8_t> f(21 + 10 + tiles.size() * 8 + dataSize);
                const auto idBytes = echonode::common::uuidToBytes(taskId);
                for (int i = 0; i < 16; ++i) f[i] = idBytes[i];
                f[16] = (seq >> 24) & 0xFF;
                f[17] = (seq >> 16) & 0xFF;
                f[18] = (seq >> 8) & 0xFF;
                f[19] = seq & 0xFF;
                f[20] = 0x06; // 桌面增量流
                auto put16 = [&](size_t off, int v) {
                    f[off] = (v >> 8) & 0xFF;
                    f[off + 1] = v & 0xFF;
                };
                put16(21, cols);
                put16(23, rows);
                put16(25, tw);
                put16(27, th);
                put16(29, static_cast<int>(tiles.size()));
                size_t meta = 31;
                size_t data = 31 + tiles.size() * 8; // 元数据区之后才是数据区
                for (size_t i = 0; i < tiles.size(); ++i) {
                    put16(meta, tiles[i].first);
                    put16(meta + 2, tiles[i].second);
                    const uint32_t len =
                        static_cast<uint32_t>(jpegs[i].size());
                    f[meta + 4] = (len >> 24) & 0xFF;
                    f[meta + 5] = (len >> 16) & 0xFF;
                    f[meta + 6] = (len >> 8) & 0xFF;
                    f[meta + 7] = len & 0xFF;
                    meta += 8;
                    std::memcpy(f.data() + data, jpegs[i].data(),
                                jpegs[i].size());
                    data += jpegs[i].size();
                }
                binarySender_(f.data(), f.size());
            };

            std::vector<uint8_t> rgb; // 复用容量，避免每帧重新分配 2.7MB

            // ECHONODE_DESK_STATS 置位时每 5s 打印取帧/扫描/编码分段耗时
            const bool statsOn = std::getenv("ECHONODE_DESK_STATS") != nullptr;
            double accCap = 0, accScan = 0, accEnc = 0;
            size_t accN = 0, accFresh = 0, accTiles = 0;
            auto accStart = std::chrono::steady_clock::now();
            auto msTo = [](std::chrono::steady_clock::time_point a,
                           std::chrono::steady_clock::time_point b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };

            while (running_) {
                const auto tickStart = std::chrono::steady_clock::now();
                const auto next = tickStart + interval;
                const auto tCap0 = tickStart;
                int w = 0, h = 0;
                bool fresh = false;
                try {
                    fresh = capture->captureScaledRgb(rgb, w, h, scale_);
                } catch (const std::exception& e) {
                    std::cerr << "[desk] capture error: " << e.what()
                              << std::endl;
                    continue;
                }
                const auto tCap1 = std::chrono::steady_clock::now();
                // 首帧未就绪时 rgb 内容未定义，不能扫描/建网格，跳过等平台给出真画面
                if (!fresh && rgb.empty()) continue;

                // 本帧元数据（仅 GPU 路径权威）：脏矩形决定发哪些 tile，移动块走 0x07
                std::vector<echonode::platform::DirtyRect> dirtyRects;
                std::vector<echonode::platform::MoveRect> moveRects;
                const bool haveMeta =
                    fresh && capture->lastFrameMetadata(dirtyRects, moveRects);

                // 分辨率变化：重建 tile 网格并强制全量
                if (w != frameW || h != frameH) {
                    frameW = w;
                    frameH = h;
                    gridW = (w + kTile - 1) / kTile;
                    gridH = (h + kTile - 1) / kTile;
                    const size_t n = static_cast<size_t>(gridW) * gridH;
                    tileSums.assign(n, 0xFFFFFFFFFFFFFFFFULL); // 哨兵：强制所有 tile 首发必发
                    queued.assign(n, 0);
                    pending.clear();
                    pendingHead = 0;
                }

                // 脏检测：每 tile 全字节和（720p 全帧遍历 ~1-2ms）
                // fresh=false 且非关键帧时不重扫（桌面确实没变），但队列里的欠账照旧往下发
                constexpr size_t kTickBudget = 48; // 每 tick 最多编码/发送的 tile 数
                const bool keyframe = (seq % 600 == 0); // 周期性关键帧防残缺
                if (fresh || keyframe) {
                    if (keyframe) {
                        // 关键帧重新排队全部 tile，不依赖校验和（丢帧/重连会留缺口）
                        std::fill(queued.begin(), queued.end(), 0);
                        pending.clear();
                        pendingHead = 0;
                    }
                    if (haveMeta && !keyframe) {
                        // 脏矩形→标记相交 tile，免去全帧字节和扫描
                        for (const auto& dr : dirtyRects) {
                            const int tx0 = std::max(0, dr.x / kTile);
                            const int ty0 = std::max(0, dr.y / kTile);
                            const int tx1 = std::min(gridW - 1, (dr.x + dr.w - 1) / kTile);
                            const int ty1 = std::min(gridH - 1, (dr.y + dr.h - 1) / kTile);
                            for (int ty = ty0; ty <= ty1; ++ty)
                                for (int tx = tx0; tx <= tx1; ++tx) {
                                    const size_t idx = static_cast<size_t>(ty) * gridW + tx;
                                    if (!queued[idx]) {
                                        queued[idx] = 1;
                                        pending.emplace_back(tx, ty);
                                    }
                                }
                        }
                    } else {
                        for (int ty = 0; ty < gridH; ++ty) {
                            for (int tx = 0; tx < gridW; ++tx) {
                                const int x0 = tx * kTile;
                                const int y0 = ty * kTile;
                                const int x1 = std::min(x0 + kTile, w);
                                const int y1 = std::min(y0 + kTile, h);
                                uint64_t sum = 0;
                                for (int y = y0; y < y1; ++y) {
                                    const uint8_t* p =
                                        &rgb[static_cast<size_t>(y) * w * 3 +
                                             static_cast<size_t>(x0) * 3];
                                    for (int x = x0; x < x1; ++x) {
                                        sum += p[0] + p[1] + p[2];
                                        p += 3;
                                    }
                                }
                                const size_t idx =
                                    static_cast<size_t>(ty) * gridW + tx;
                                if (keyframe || sum != tileSums[idx]) {
                                    tileSums[idx] = sum;
                                    if (!queued[idx]) {
                                        queued[idx] = 1;
                                        pending.emplace_back(tx, ty);
                                    }
                                }
                            }
                        }
                    }
                }

                const auto tScan1 = std::chrono::steady_clock::now();
                std::vector<std::pair<int, int>> dirty;
                while (dirty.size() < kTickBudget && pendingHead < pending.size()) {
                    const auto t = pending[pendingHead++];
                    queued[static_cast<size_t>(t.second) * gridW + t.first] = 0;
                    dirty.push_back(t);
                }
                if (pendingHead == pending.size()) { // 已全部发出，回收内存从头部重来
                    pending.clear();
                    pendingHead = 0;
                }

                // 光标（RDP/VNC 模式）：位置 0x04 小帧 + 形状变化发 0x05 PNG
                int curX = 0, curY = 0, capW = 0, capH = 0;
                if (getCursorLogical(curX, curY, capW, capH)) {
                    const double cnx = static_cast<double>(curX) / capW;
                    const double cny = static_cast<double>(curY) / capH;
                    if (cnx != lastCx || cny != lastCy) {
                        lastCx = cnx;
                        lastCy = cny;
                        if (binarySender_) {
                            std::vector<uint8_t> c(21 + 8);
                            const auto idBytes =
                                echonode::common::uuidToBytes(taskId);
                            for (int i = 0; i < 16; ++i) c[i] = idBytes[i];
                            c[20] = 0x04;
                            float xy[2] = {static_cast<float>(cnx),
                                           static_cast<float>(cny)};
                            std::memcpy(c.data() + 21, xy, 8);
                            binarySender_(c.data(), c.size());
                        }
                    }
                    void* shapeId = nullptr;
                    std::vector<uint8_t> rgba;
                    int cw = 0, ch = 0, hx = 0, hy = 0;
                    if (getCursorImage(rgba, cw, ch, hx, hy, &shapeId) &&
                        shapeId != lastShape && binarySender_) {
                        lastShape = shapeId;
                        std::vector<uint8_t> png;
                        if (stbi_write_png_to_func(stbAppend, &png, cw, ch, 4,
                                                   rgba.data(), cw * 4) &&
                            !png.empty()) {
                            std::vector<uint8_t> f(21 + 8 + png.size());
                            const auto idBytes =
                                echonode::common::uuidToBytes(taskId);
                            for (int i = 0; i < 16; ++i) f[i] = idBytes[i];
                            f[20] = 0x05;
                            f[21] = (hx >> 8) & 0xFF;
                            f[22] = hx & 0xFF;
                            f[23] = (hy >> 8) & 0xFF;
                            f[24] = hy & 0xFF;
                            f[25] = (cw >> 8) & 0xFF;
                            f[26] = cw & 0xFF;
                            f[27] = (ch >> 8) & 0xFF;
                            f[28] = ch & 0xFF;
                            std::memcpy(f.data() + 29, png.data(), png.size());
                            binarySender_(f.data(), f.size());
                        }
                    }
                }

                // 发送 0x07 移动块（先于脏 tile，前端先搬移再叠新内容）
                if (haveMeta && !keyframe && !moveRects.empty() && binarySender_) {
                    std::vector<uint8_t> mf(23 + moveRects.size() * 12);
                    const auto idBytes = echonode::common::uuidToBytes(taskId);
                    for (int i = 0; i < 16; ++i) mf[i] = idBytes[i];
                    mf[16] = (seq >> 24) & 0xFF; mf[17] = (seq >> 16) & 0xFF;
                    mf[18] = (seq >> 8) & 0xFF; mf[19] = seq & 0xFF;
                    mf[20] = 0x07;
                    auto put16 = [&](size_t off, int v) {
                        mf[off] = (v >> 8) & 0xFF; mf[off + 1] = v & 0xFF;
                    };
                    put16(21, static_cast<int>(moveRects.size()));
                    size_t o = 23;
                    for (const auto& m : moveRects) {
                        put16(o, m.sx); put16(o + 2, m.sy); put16(o + 4, m.dx);
                        put16(o + 6, m.dy); put16(o + 8, m.w); put16(o + 10, m.h);
                        o += 12;
                    }
                    binarySender_(mf.data(), mf.size());
                }

                // 编码并发送脏 tile（无脏 tile 则不发）；分批发送防单帧过大
                if (!dirty.empty()) {
                    constexpr size_t kMaxPerFrame = 24;
                    std::vector<std::vector<uint8_t>> jpegs;
                    encodeTilesParallel(rgb, w, h, dirty, quality, kTile, jpegs);
                    std::vector<std::pair<int, int>> batch;
                    std::vector<std::vector<uint8_t>> batchJpegs;
                    batch.reserve(kMaxPerFrame);
                    batchJpegs.reserve(kMaxPerFrame);
                    for (size_t i = 0; i < dirty.size(); ++i) {
                        batch.push_back(dirty[i]);
                        batchJpegs.emplace_back(std::move(jpegs[i]));
                        if (batch.size() >= kMaxPerFrame ||
                            i + 1 == dirty.size()) {
                            sendDelta(gridW, gridH, kTile, kTile, batch,
                                      batchJpegs);
                            batch.clear();
                            batchJpegs.clear();
                        }
                    }
                }

                if (statsOn) {
                    const auto tEnd = std::chrono::steady_clock::now();
                    accCap += msTo(tCap0, tCap1);
                    accScan += msTo(tCap1, tScan1);
                    accEnc += msTo(tScan1, tEnd);
                    ++accN;
                    accFresh += fresh ? 1 : 0;
                    accTiles += dirty.size();
                    if (tEnd - accStart >= std::chrono::seconds(5)) {
                        std::fprintf(stderr,
                                     "[desk-stats] tick=%zu fresh=%zu tiles=%zu "
                                     "cap=%.1f scan=%.1f enc=%.1f(ms/tick)\n",
                                     accN, accFresh, accTiles, accCap / accN,
                                     accScan / accN, accEnc / accN);
                        accCap = accScan = accEnc = 0;
                        accN = accFresh = accTiles = 0;
                        accStart = tEnd;
                    }
                }

                ++seq;
                std::this_thread::sleep_until(next);
            }
        } catch (const std::exception&) {
            // 抓屏对象创建失败：线程退出，operator 靠超时感知
        }
        running_ = false;
#ifdef _WIN32
        timeEndPeriod(1);
#endif
    });
}

void RemoteDeskExecutor::stopLoop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

}
