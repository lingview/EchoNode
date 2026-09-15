// 交互式 shell 会话管理：会话表 + 后台 pump 线程，输出经 OutputSink 推回服务端
#pragma once
#include "../platform/IShellSpawn.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace echonode::executor {

class ShellSessionManager {
public:
    // data 为会话输出；eof=true 表示会话已结束，此后 sessionId 不再有效
    using OutputSink = std::function<void(const std::string& sessionId,
                                          const std::string& data, bool eof)>;

    explicit ShellSessionManager(std::unique_ptr<platform::IShellSpawn> shell);
    ~ShellSessionManager(); // 停 pump 并关闭全部会话

    void setSink(OutputSink sink);

    // 启动长驻 shell；shellPath 为空时用平台默认；返回 sessionId，失败抛 PlatformError
    std::string open(const std::string& shellPath);

    // 向会话 stdin 写入；未知 sessionId 返回 false
    bool input(const std::string& sessionId, const std::string& data);

    // 关闭会话；未知 sessionId 静默忽略
    void close(const std::string& sessionId);

    // 调整会话终端尺寸（列/行）；未知 sessionId 返回 false
    bool resize(const std::string& sessionId, int cols, int rows);

private:
    void pumpLoop();

    std::unique_ptr<platform::IShellSpawn> shell_;
    std::string defaultShell_;
    OutputSink sink_;

    std::mutex mtx_;
    std::map<std::string, platform::ShellHandle> sessions_;
    // 各会话的 UTF-8 残字节缓冲（读取块边界可能切在多字节字符中间）
    std::map<std::string, std::string> pending_;

    std::atomic<bool> running_{false};
    std::thread pump_;
};

}
