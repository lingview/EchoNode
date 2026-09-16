#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace echonode::net {

struct WsBackend;

class WsClient {
public:
    using OpenHandler = std::function<void()>;
    using TextHandler = std::function<void(const std::string&)>;
    using BinaryHandler = std::function<void(const void*, size_t)>;
    using CloseHandler = std::function<void()>;

    WsClient(std::string url, int backoffBaseMs = 1000, int backoffCapMs = 60000);
    ~WsClient();

    void setHandlers(OpenHandler onOpen, TextHandler onText, CloseHandler onClose);
    void setBinaryHandler(BinaryHandler onBinary);
    void start();
    void stop();

    bool sendText(const std::string& text);
    bool sendBinary(const void* data, size_t len);

private:
    std::unique_ptr<WsBackend> impl_;
};

} // namespace echonode::net
