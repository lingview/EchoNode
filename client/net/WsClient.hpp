// WebSocket 连接管理：连接、指数退避重连、收发
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace echonode::net {

class WsClient {
public:
    using OpenHandler = std::function<void()>;
    using TextHandler = std::function<void(const std::string&)>;   // text frame
    using BinaryHandler = std::function<void(const void*, size_t)>; // binary frame
    using CloseHandler = std::function<void()>;

    // url 形如 ws://host:port；退避从 base 翻倍到 cap
    WsClient(std::string url, int backoffBaseMs = 1000, int backoffCapMs = 60000);
    ~WsClient(); // 析构自动 stop

    void setHandlers(OpenHandler onOpen, TextHandler onText, CloseHandler onClose);
    void setBinaryHandler(BinaryHandler onBinary);
    void start(); // 启动网络线程，立即返回
    void stop();  // 停止并断开，阻塞至线程退出

    // 未连接或已停止返回 false
    bool sendText(const std::string& text);
    bool sendBinary(const void* data, size_t len);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace echonode::net
