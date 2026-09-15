#pragma once
#include "Config.hpp"

#include "protocol/from_server.hpp"
#include "protocol/to_server.hpp"

#include <functional>
#include <nlohmann/json.hpp>

namespace echonode::core {

class Client {
public:
    using TaskHandler = std::function<protocol::TaskResult(protocol::Task)>;

    using TextHook = std::function<bool(const nlohmann::json&)>;
    using BinaryHook = std::function<bool(const void*, size_t)>;

    explicit Client(Config cfg);
    ~Client();

    void setTaskHandler(TaskHandler handler);
    void setTextHook(TextHook hook);
    void setBinaryHook(BinaryHook hook);
    void run();
    void stop();
    bool sendText(const std::string& text);
    bool sendBinary(const void* data, size_t len);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace echonode::core
