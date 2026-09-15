// 远程桌面执行器：
// remote_start  抓屏线程按 fps 定时：captureBmp → RGB → JPEG → binary 帧(type 0x03)
// remote_stop   停线程
// remote_input  输入事件注入（走 task 通道高频到达，server 侧不落库）
// binary 帧布局：16B taskId + 4B seq + 1B type(0x03) + JPEG 数据
#pragma once
#include "IExecutor.hpp"
#include "platform/IInputInjector.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace echonode::executor {

class RemoteDeskExecutor : public IExecutor {
public:
    using BinarySender = std::function<void(const void*, size_t)>;
    using ResultSender = std::function<void(const protocol::TaskResult&)>;
    using InjectorPtr = std::shared_ptr<platform::IInputInjector>;

    void setBinarySender(BinarySender sender) { binarySender_ = std::move(sender); }
    void setResultSender(ResultSender sender) { resultSender_ = std::move(sender); }
    void setInputInjector(InjectorPtr inj) { injector_ = std::move(inj); }

    std::vector<std::string> actions() const override;
    protocol::TaskResult execute(protocol::Task task) override;
    ~RemoteDeskExecutor() override;

private:
    void startLoop(const std::string& taskId, int fps, int quality);
    void stopLoop();

    BinarySender binarySender_;
    ResultSender resultSender_;
    InjectorPtr injector_;

    std::thread worker_;
    std::atomic<bool> running_{false};
    std::string streamTaskId_; // 当前流的 taskId（帧头用）
    int fps_ = 8;
    int quality_ = 60;
    double scale_ = 0.5; // 降采样系数（主要优化：像素 ÷4，编码时间同步 ÷4）
};

}
