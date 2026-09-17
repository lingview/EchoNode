// 远程桌面执行器：抓屏线程按质量阶梯自适应出帧，浏览器经 desk_stat 回传驱动升降档
#pragma once
#include "IExecutor.hpp"
#include "platform/IInputInjector.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace echonode::executor {

class RemoteDeskExecutor : public IExecutor {
public:
    using BinarySender = std::function<void(const void*, size_t)>;
    using ResultSender = std::function<void(const protocol::TaskResult&)>;
    using InjectorPtr = std::shared_ptr<platform::IInputInjector>;

    void setBinarySender(BinarySender sender) { binarySender_ = std::move(sender); }
    void setResultSender(ResultSender sender) { resultSender_ = std::move(sender); }
    void setInputInjector(InjectorPtr inj) { injector_ = std::move(inj); }
    void setStatsEnabled(bool on) { statsEnabled_ = on; }

    void onDeskStat(int recvFps, int stallMs, uint32_t lastSeq);

    std::vector<std::string> actions() const override;
    protocol::TaskResult execute(protocol::Task task) override;
    ~RemoteDeskExecutor() override;

private:
    struct QLevel {
        double scale;
        int quality;
        int fps;
    };
    void startLoop(std::string taskId, std::vector<QLevel> ladder);
    void stopLoop();

    BinarySender binarySender_;
    ResultSender resultSender_;
    InjectorPtr injector_;
    bool statsEnabled_ = false;

    std::thread worker_;
    std::atomic<bool> running_{false};

    std::atomic<int> statFps_{-1};
    std::atomic<int> statStallMs_{-1};
    std::atomic<uint32_t> statSeq_{0};
    std::atomic<bool> statValid_{false};
};

} // namespace echonode::executor
