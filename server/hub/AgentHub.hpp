#pragma once
#include "../db/Database.hpp"
#include "../ws/WsGateway.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace echonode::server {

class AgentHub {
public:
    using EventNotifier = std::function<void(const nlohmann::json& event)>;
    using MessageHandler = std::function<void(const std::string& agentId, const std::string& text)>;
    using BinaryHandler = std::function<void(const std::string& agentId, const void* data, size_t len)>;

    AgentHub(WsGateway& gw, std::string agentToken, Database* db = nullptr);
    ~AgentHub();

    void startHeartbeatWatch();
    std::string agentsJson() const;

    void setOnlineNotifier(EventNotifier notifier);
    void setOnAgentMessage(MessageHandler handler);
    void setOnAgentBinary(BinaryHandler handler);

    bool sendTextToAgent(const std::string& agentId, const std::string& text);
    bool sendBinaryToAgent(const std::string& agentId, const void* data, size_t len);

private:
    struct Agent {
        std::string agentId;
        std::string hostname, osName, osVersion, userName, arch;
        std::chrono::steady_clock::time_point lastSeen;
        WsHdl hdl;
    };

    void onOpen(WsHdl hdl);
    void onText(WsHdl hdl, const std::string& text);
    void onBinary(WsHdl hdl, const void* data, size_t len);
    void onClose(WsHdl hdl);
    void watchLoop();

    WsGateway& gw_;
    std::string agentToken_;
    Database* db_;
    EventNotifier notifier_;
    MessageHandler onMessage_;
    BinaryHandler onBinary_;
    mutable std::mutex mtx_;
    std::map<std::string, Agent> agents_;
    std::map<WsHdl, std::string, HdlLess> connToId_;
    std::atomic<bool> running_{false};
    std::thread watcher_;
};

} // namespace echonode::server
