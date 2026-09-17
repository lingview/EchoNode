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
#include <mmsystem.h>
#else
#include <X11/Xlib.h>
#endif

namespace echonode::executor {

using protocol::Task;
using protocol::TaskResult;

namespace {

constexpr int kTile = 64;

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

        bool hasAlpha = false;
        if (is32)
            for (size_t i = 0; i < color.size(); i += 4)
                if (color[i + 3]) { hasAlpha = true; break; }
        const bool premult = is32 && hasAlpha;
        std::vector<uint8_t> andBits;

        if (!premult) extractBits(ii.hbmMask, 0, h, andBits);
        for (int y = 0; y < h; ++y) {
            const int srcY = h - 1 - y;
            for (int x = 0; x < w; ++x) {
                const uint8_t* s =
                    &color[(static_cast<size_t>(srcY) * w + x) * 4];
                uint8_t* d = &rgba[(static_cast<size_t>(y) * w + x) * 4];
                if (premult) {
                    const int a = s[3];
                    d[3] = static_cast<uint8_t>(a);
                    if (a > 0) {
                        d[0] = static_cast<uint8_t>(std::min(255, s[2] * 255 / a));
                        d[1] = static_cast<uint8_t>(std::min(255, s[1] * 255 / a));
                        d[2] = static_cast<uint8_t>(std::min(255, s[0] * 255 / a));
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
                    const bool black = s[0] == 0 && s[1] == 0 && s[2] == 0;
                    d[3] = (andBit && black) ? 0 : 255;
                }
            }
        }
    } else {
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

void stbAppend(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(ctx);
    const auto* p = static_cast<const uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

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

void RemoteDeskExecutor::onDeskStat(int recvFps, int stallMs, uint32_t lastSeq) {
    statFps_.store(recvFps, std::memory_order_relaxed);
    statStallMs_.store(stallMs, std::memory_order_relaxed);
    statSeq_.store(lastSeq, std::memory_order_relaxed);
    statValid_.store(true, std::memory_order_relaxed);
}

TaskResult RemoteDeskExecutor::execute(Task task) {
    if (task.action == "remote_start") {
        const int reqFps = std::clamp(task.payload.value("fps", 24), 1, 60);
        const int reqQuality = std::clamp(task.payload.value("quality", 60), 20, 95);
        const double scale = std::clamp(task.payload.value("scale", 0.5), 0.25, 1.0);
        stopLoop();

        std::vector<QLevel> ladder;
        for (int i = 0; i < 6; ++i) {
            ladder.push_back({scale, std::max(24, reqQuality - i * 9),
                              std::max(8, reqFps - i * 5)});
        }

        int w = 0, h = 0;
        try {
            auto capture = echonode::platform::createScreenCapture();
            std::vector<uint8_t> rgb;
            for (int i = 0; i < 8 && rgb.empty(); ++i) {
                capture->captureScaledRgb(rgb, w, h, scale);
                if (rgb.empty())
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (rgb.empty() || w <= 0 || h <= 0)
                return {task.taskId, false, {}, "screen capture returned no pixels"};
        } catch (const std::exception&) {
            return {task.taskId, false, {}, "screen capture unavailable"};
        }

        startLoop(task.taskId, std::move(ladder));
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
        for (const auto& ev : task.payload.value("events", nlohmann::json::array())) {
            const std::string kind = ev.value("kind", "");
            if (kind == "move") {
                injector_->mouseMove(ev.value("nx", 0.0), ev.value("ny", 0.0));
            } else if (kind == "button") {
                injector_->mouseButton(ev.value("button", 0), ev.value("down", true));
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

void RemoteDeskExecutor::startLoop(std::string taskId, std::vector<QLevel> ladder) {
    statValid_.store(false);
    running_ = true;
    worker_ = std::thread([this, taskId, ladder = std::move(ladder)] {
#ifdef _WIN32
        timeBeginPeriod(1);
#endif
        const double scale = ladder.front().scale;
        size_t level = 0;
        bool forceFull = false;
        uint32_t seq = 0;
        uint32_t tick = 0;
        size_t sentThisWindow = 0;
        double lastCx = -1, lastCy = -1;
        void* lastShape = nullptr;

        auto lastCtl = std::chrono::steady_clock::now();
        int goodStreak = 0;
        int badStreak = 0;
        int lastRf = -1, lastStall = -1, lastSupply = 0;

        try {
            auto capture = echonode::platform::createScreenCapture();

            int gridW = 0, gridH = 0, frameW = 0, frameH = 0;
            std::vector<uint64_t> tileSums;
            std::vector<std::pair<int, int>> pending;
            std::vector<uint8_t> queued;
            size_t pendingHead = 0;

            auto sendDelta = [&](int cols, int rows, int tw, int th,
                                 const std::vector<std::pair<int, int>>& tiles,
                                 const std::vector<std::vector<uint8_t>>& jpegs) {
                if (!binarySender_) return;
                size_t dataSize = 0;
                for (const auto& j : jpegs) dataSize += j.size();
                std::vector<uint8_t> f(21 + 10 + tiles.size() * 8 + dataSize);
                const auto idBytes = echonode::common::uuidToBytes(taskId);
                for (int i = 0; i < 16; ++i) f[i] = idBytes[i];
                f[16] = (seq >> 24) & 0xFF;
                f[17] = (seq >> 16) & 0xFF;
                f[18] = (seq >> 8) & 0xFF;
                f[19] = seq & 0xFF;
                f[20] = 0x06;
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
                size_t data = 31 + tiles.size() * 8;
                for (size_t i = 0; i < tiles.size(); ++i) {
                    put16(meta, tiles[i].first);
                    put16(meta + 2, tiles[i].second);
                    const uint32_t len = static_cast<uint32_t>(jpegs[i].size());
                    f[meta + 4] = (len >> 24) & 0xFF;
                    f[meta + 5] = (len >> 16) & 0xFF;
                    f[meta + 6] = (len >> 8) & 0xFF;
                    f[meta + 7] = len & 0xFF;
                    meta += 8;
                    std::memcpy(f.data() + data, jpegs[i].data(), jpegs[i].size());
                    data += jpegs[i].size();
                }
                binarySender_(f.data(), f.size());
                ++sentThisWindow;
            };

            std::vector<uint8_t> rgb;

            const bool statsOn =
                statsEnabled_ || std::getenv("ECHONODE_DESK_STATS") != nullptr;
            auto msTo = [](std::chrono::steady_clock::time_point a,
                           std::chrono::steady_clock::time_point b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };
            double accCap = 0, accScan = 0, accEnc = 0;
            size_t accN = 0, accFresh = 0, accTiles = 0;
            auto accStart = std::chrono::steady_clock::now();

            while (running_) {
                const auto tickStart = std::chrono::steady_clock::now();
                const auto interval =
                    std::chrono::microseconds(1000000 / std::max(1, ladder[level].fps));
                const auto next = tickStart + interval;
                const auto tCap0 = tickStart;

                int w = 0, h = 0;
                bool fresh = false;
                try {
                    fresh = capture->captureScaledRgb(rgb, w, h, scale);
                } catch (const std::exception& e) {
                    std::cerr << "[desk] capture error: " << e.what() << std::endl;
                    continue;
                }
                const auto tCap1 = std::chrono::steady_clock::now();
                if (!fresh && rgb.empty()) continue;

                std::vector<echonode::platform::DirtyRect> dirtyRects;
                std::vector<echonode::platform::MoveRect> moveRects;
                const bool haveMeta =
                    fresh && capture->lastFrameMetadata(dirtyRects, moveRects);

                if (w != frameW || h != frameH) {
                    frameW = w;
                    frameH = h;
                    gridW = (w + kTile - 1) / kTile;
                    gridH = (h + kTile - 1) / kTile;
                    const size_t n = static_cast<size_t>(gridW) * gridH;
                    tileSums.assign(n, 0xFFFFFFFFFFFFFFFFULL);
                    queued.assign(n, 0);
                    pending.clear();
                    pendingHead = 0;
                }

                const size_t backlog = pending.size() - pendingHead;
                const size_t fullTiles = static_cast<size_t>(gridW) * gridH;
                if (fullTiles > 0 && backlog > fullTiles * 2) {
                    pending.clear();
                    pendingHead = 0;
                    std::fill(queued.begin(), queued.end(), 0);
                    forceFull = true;
                }

                constexpr size_t kTickBudget = 48;
                const bool keyframe = (tick % 600 == 0) || forceFull;
                forceFull = false;
                if (fresh || keyframe) {
                    if (keyframe) {
                        std::fill(queued.begin(), queued.end(), 0);
                        pending.clear();
                        pendingHead = 0;
                    }
                    if (haveMeta && !keyframe) {
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
                                const size_t idx = static_cast<size_t>(ty) * gridW + tx;
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
                if (pendingHead == pending.size()) {
                    pending.clear();
                    pendingHead = 0;
                }

                int curX = 0, curY = 0, capW = 0, capH = 0;
                if (getCursorLogical(curX, curY, capW, capH)) {
                    const double cnx = static_cast<double>(curX) / capW;
                    const double cny = static_cast<double>(curY) / capH;
                    if (cnx != lastCx || cny != lastCy) {
                        lastCx = cnx;
                        lastCy = cny;
                        if (binarySender_) {
                            std::vector<uint8_t> c(21 + 8);
                            const auto idBytes = echonode::common::uuidToBytes(taskId);
                            for (int i = 0; i < 16; ++i) c[i] = idBytes[i];
                            c[20] = 0x04;
                            float xy[2] = {static_cast<float>(cnx), static_cast<float>(cny)};
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
                            const auto idBytes = echonode::common::uuidToBytes(taskId);
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

                bool sentContent = false;
                if (haveMeta && !keyframe && !moveRects.empty() && binarySender_) {
                    std::vector<uint8_t> mf(23 + moveRects.size() * 12);
                    const auto idBytes = echonode::common::uuidToBytes(taskId);
                    for (int i = 0; i < 16; ++i) mf[i] = idBytes[i];
                    mf[16] = (seq >> 24) & 0xFF;
                    mf[17] = (seq >> 16) & 0xFF;
                    mf[18] = (seq >> 8) & 0xFF;
                    mf[19] = seq & 0xFF;
                    mf[20] = 0x07;
                    auto put16 = [&](size_t off, int v) {
                        mf[off] = (v >> 8) & 0xFF;
                        mf[off + 1] = v & 0xFF;
                    };
                    put16(21, static_cast<int>(moveRects.size()));
                    size_t o = 23;
                    for (const auto& m : moveRects) {
                        put16(o, m.sx);
                        put16(o + 2, m.sy);
                        put16(o + 4, m.dx);
                        put16(o + 6, m.dy);
                        put16(o + 8, m.w);
                        put16(o + 10, m.h);
                        o += 12;
                    }
                    binarySender_(mf.data(), mf.size());
                    ++sentThisWindow;
                    sentContent = true;
                }

                const int quality = ladder[level].quality;
                if (!dirty.empty()) {
                    sentContent = true;
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
                        if (batch.size() >= kMaxPerFrame || i + 1 == dirty.size()) {
                            sendDelta(gridW, gridH, kTile, kTile, batch, batchJpegs);
                            batch.clear();
                            batchJpegs.clear();
                        }
                    }
                }


                const auto now = std::chrono::steady_clock::now();
                const std::chrono::duration<double> ctlElapsed = now - lastCtl;

                if (!statValid_.load(std::memory_order_relaxed)) {
                    lastCtl = now;
                    sentThisWindow = 0;
                } else if (ctlElapsed >= std::chrono::milliseconds(1000)) {
                    lastCtl = now;
                    if (statValid_.load(std::memory_order_relaxed) &&
                        sentThisWindow > 0 && ctlElapsed.count() > 0) {
                        const int rf = statFps_.load(std::memory_order_relaxed);
                        const int st = statStallMs_.load(std::memory_order_relaxed);
                        const int supplyFps = static_cast<int>(
                            sentThisWindow / ctlElapsed.count() + 0.5);
                        lastRf = rf; lastStall = st; lastSupply = supplyFps;

                        if (supplyFps >= 5) {

                            const bool bad = rf >= 0 && rf * 100 < supplyFps * 60;
                            const bool good = rf * 100 >= supplyFps * 90;
                            if (bad) {
                                goodStreak = 0;
                                if (++badStreak >= 2 && level + 1 < ladder.size()) {
                                    ++level;
                                    forceFull = true;
                                    badStreak = 0;
                                }
                            } else {
                                badStreak = 0;
                                if (good && level > 0 && ++goodStreak >= 2) {
                                    --level;
                                    goodStreak = 0;
                                }
                            }
                        }
                    }
                    sentThisWindow = 0;
                }

                if (sentContent) ++seq;
                ++tick;

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
                                     "[desk-stats] level=%zu cap=%.1f scan=%.1f "
                                     "enc=%.1f tiles=%zu rf=%d stall=%d sup=%d full=%dx%d\n",
                                     level, accCap / accN, accScan / accN,
                                     accEnc / accN, accTiles, lastRf, lastStall,
                                     lastSupply, gridW, gridH);
                        accCap = accScan = accEnc = 0;
                        accN = accFresh = accTiles = 0;
                        accStart = tEnd;
                    }
                }

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

} // namespace echonode::executor
