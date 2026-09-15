#include "ScreenshotExecutor.hpp"

#include "platform/Error.hpp"
#include "util/Uuid.hpp"

#include <nlohmann/json.hpp>
#include "stb/stb_image_write.h"

#include <algorithm>

namespace echonode::executor {

namespace {
// stb 内存编码回调：把编码结果追加到 vector
void stbAppend(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<uint8_t>*>(ctx);
    const auto* p = static_cast<const uint8_t*>(data);
    out->insert(out->end(), p, p + size);
}

// 把 captureBmp 产出的 BMP(32bpp 底向上) 转成顶到底 RGB24
bool bmpToRgb24(const std::vector<uint8_t>& bmp, std::vector<uint8_t>& rgb,
                int& w, int& h) {
    if (bmp.size() < 54 || bmp[0] != 'B' || bmp[1] != 'M') return false;
    auto rd32 = [&](size_t o) -> uint32_t {
        return static_cast<uint32_t>(bmp[o]) | (static_cast<uint32_t>(bmp[o + 1]) << 8) |
               (static_cast<uint32_t>(bmp[o + 2]) << 16) |
               (static_cast<uint32_t>(bmp[o + 3]) << 24);
    };
    auto rd16 = [&](size_t o) -> uint16_t {
        return static_cast<uint16_t>(bmp[o] | (bmp[o + 1] << 8));
    };
    const uint32_t dataOff = rd32(10);
    const auto width = static_cast<int32_t>(rd32(18));
    const auto height = static_cast<int32_t>(rd32(22));
    const uint16_t bpp = rd16(28);
    if (bpp != 32 || width <= 0 || height <= 0) return false;
    if (bmp.size() < dataOff + static_cast<size_t>(width) * height * 4) return false;

    w = width;
    h = height;
    rgb.resize(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        const uint8_t* src =
            &bmp[dataOff + static_cast<size_t>(height - 1 - y) * width * 4];
        uint8_t* dst = &rgb[static_cast<size_t>(y) * width * 3];
        for (int x = 0; x < width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 2]; // R
            dst[x * 3 + 1] = src[x * 4 + 1]; // G
            dst[x * 3 + 2] = src[x * 4 + 0]; // B
        }
    }
    return true;
}
} // namespace

// 截取全屏编码为 JPEG，按 0x02 流分块推送，返回 size/sha256
protocol::TaskResult ScreenshotExecutor::execute(protocol::Task task) {
    try {
        if (!binarySender_) {
            return {task.taskId, false, {}, "binary sender not configured"};
        }
        const int quality = std::clamp(task.payload.value("quality", 90), 50, 100);

        // 走纯 GDI 的 captureBmp，不用 DXGI 通道，避免与远程桌面争抢 duplication
        const auto bmp = capture_->captureBmp();
        std::vector<uint8_t> rgb;
        int w = 0, h = 0;
        if (!bmpToRgb24(bmp, rgb, w, h)) {
            return {task.taskId, false, {}, "BMP parse failed"};
        }

        std::vector<uint8_t> jpeg;
        if (!stbi_write_jpg_to_func(stbAppend, &jpeg, w, h, 3, rgb.data(), quality) ||
            jpeg.empty()) {
            return {task.taskId, false, {}, "JPEG encode failed"};
        }

        const std::string taskIdBytes = common::uuidToBytes(task.taskId);
        common::Sha256 sha;
        uint32_t seq = 0;
        size_t offset = 0;

        // 帧头：16B taskId + 4B seq(大端) + 1B 流类型(0x02)
        while (offset < jpeg.size()) {
            const size_t take = std::min(kChunkSize, jpeg.size() - offset);
            sha.update(jpeg.data() + offset, take);
            std::string frame = taskIdBytes;
            frame += static_cast<char>((seq >> 24) & 0xFF);
            frame += static_cast<char>((seq >> 16) & 0xFF);
            frame += static_cast<char>((seq >> 8) & 0xFF);
            frame += static_cast<char>(seq & 0xFF);
            frame += static_cast<char>(kStreamScreenshot);
            frame.append(reinterpret_cast<const char*>(jpeg.data()) + offset, take);
            binarySender_(frame.data(), frame.size());
            offset += take;
            ++seq;
        }

        nlohmann::json summary = {{"size", jpeg.size()}, {"sha256", sha.finish()}};
        return {task.taskId, true, summary.dump(), {}};
    } catch (const platform::PlatformError& e) {
        return {task.taskId, false, {}, e.what()};
    }
}

} // namespace echonode::executor
