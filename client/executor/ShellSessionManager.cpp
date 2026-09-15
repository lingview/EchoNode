#include "ShellSessionManager.hpp"

#include "../platform/ISystemInfo.hpp"
#include "../platform/PlatformFactory.hpp"
#include "util/Uuid.hpp"

#include <chrono>
#include <iostream>

namespace echonode::executor {

ShellSessionManager::ShellSessionManager(std::unique_ptr<platform::IShellSpawn> shell)
    : shell_(std::move(shell)) {
    // 默认 shell 按运行平台在启动时确定一次
    defaultShell_ = platform::createSystemInfo()->getHostInfo().osName == "Windows"
                        ? "cmd.exe"
                        : "/bin/bash";
    running_ = true;
    pump_ = std::thread([this] { pumpLoop(); });
}

ShellSessionManager::~ShellSessionManager() {
    running_ = false;
    if (pump_.joinable()) pump_.join();
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& [id, handle] : sessions_) {
        shell_->terminate(handle);
    }
    sessions_.clear();
}

void ShellSessionManager::setSink(OutputSink sink) {
    sink_ = std::move(sink);
}

std::string ShellSessionManager::open(const std::string& shellPath) {
;
    const std::string id = echonode::common::generateUuid();
    auto handle = shell_->spawn(shellPath.empty() ? defaultShell_ : shellPath);
    std::lock_guard<std::mutex> lk(mtx_);
    sessions_[id] = handle;
    return id;
}

bool ShellSessionManager::input(const std::string& sessionId, const std::string& data) {
    platform::ShellHandle handle = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return false;
        handle = it->second;
    }
    return shell_->write(handle, data);
}

bool ShellSessionManager::resize(const std::string& sessionId, int cols, int rows) {
    platform::ShellHandle handle = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return false;
        handle = it->second;
    }
    return shell_->resize(handle, cols, rows);
}

void ShellSessionManager::close(const std::string& sessionId) {
    platform::ShellHandle handle = nullptr;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return;
        handle = it->second;
        sessions_.erase(it);
    }
    shell_->terminate(handle);
    if (sink_) sink_(sessionId, {}, true);
}

namespace {

// 返回 s 的最长前缀长度，使前缀不以不完整的 UTF-8 多字节序列结尾（残字节留到下块拼接）
size_t safeUtf8PrefixLen(const std::string& s) {
    if (s.empty()) return 0;
    size_t back = 0;
    while (back < 4 && back < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[s.size() - 1 - back]);
        if ((c & 0xC0) != 0x80) { // 起始字节
            size_t need = 1;
            if ((c & 0xE0) == 0xC0) need = 2;
            else if ((c & 0xF0) == 0xE0) need = 3;
            else if ((c & 0xF8) == 0xF0) need = 4;
            size_t tail = back + 1; // 起始字节 + 延续字节数
            if (tail < need) return s.size() - tail; // 残字节不交付
            return s.size();
        }
        ++back;
    }
    return s.size();
}

} // namespace

void ShellSessionManager::pumpLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            const std::string& id = it->first;
            platform::ShellHandle handle = it->second;

            std::string chunk;
            bool dead = false;
            // 非阻塞读直到暂无数据；会话退出后把 eof 通知 sink
            while (shell_->tryRead(handle, chunk)) {
                // UTF-8 安全分帧：块边界可能切在多字节字符中间
                auto& pending = pending_[id];
                pending += chunk;
                const size_t cut = safeUtf8PrefixLen(pending);
                if (cut > 0) {
                    if (sink_) sink_(id, pending.substr(0, cut), false);
                    pending.erase(0, cut);
                }
                chunk.clear();
            }
            if (!shell_->alive(handle)) dead = true;

            if (dead) {
                // 死前交付残余字节
                auto it2 = pending_.find(id);
                if (it2 != pending_.end() && !it2->second.empty()) {
                    if (sink_) sink_(id, it2->second, false);
                    pending_.erase(it2);
                }
                if (sink_) sink_(id, {}, true);
                shell_->terminate(handle);
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

} // namespace echonode::executor
