// 截图执行器：全屏 JPEG 按 binary frame(流类型 0x02) 分块推送，结果汇总 size/sha256
// 质量默认 90，可由 payload.quality 覆盖（clamp [50,100]）
#pragma once
#include "IExecutor.hpp"

#include "util/Sha256.hpp"

#include "../platform/IScreenCapture.hpp"

#include <functional>
#include <memory>

namespace echonode::executor {

class ScreenshotExecutor : public IExecutor {
public:
    using BinarySender = std::function<void(const void*, size_t)>;

    explicit ScreenshotExecutor(std::unique_ptr<platform::IScreenCapture> capture)
        : capture_(std::move(capture)) {}

    std::vector<std::string> actions() const override { return {"screenshot"}; }

    protocol::TaskResult execute(protocol::Task task) override;

    void setBinarySender(BinarySender sender) { binarySender_ = std::move(sender); }

private:
    static constexpr size_t kChunkSize = 64 * 1024;
    static constexpr uint8_t kStreamScreenshot = 0x02;

    std::unique_ptr<platform::IScreenCapture> capture_;
    BinarySender binarySender_;
};

}
