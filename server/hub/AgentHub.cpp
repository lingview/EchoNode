#include "AgentHub.hpp"

#include "protocol/to_server.hpp"
#include "util/Uuid.hpp"

#include <nlohmann/json.hpp>

namespace echonode::server {

namespace {
constexpr int kHeartbeatTimeoutSec = 30;
constexpr int kWatchIntervalSec = 5;

int64_t nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

AgentHub::AgentHub(WsGateway& gw, std::string agentToken, Database* db)
    : gw_(gw), agentToken_(std::move(agentToken)), db_(db) {
    WsRoute route;
    route.onOpen = [this](WsHdl h) { onOpen(h); };
    route.onText = [this](WsHdl h, const std::string& t) { onText(h, t); };
    route.onBinary = [this](WsHdl h, const void* d, size_t l) { onBinary(h, d, l); };
    route.onClose = [this](WsHdl h) { onClose(h); };
    gw_.addRoute("/agent", std::move(route));
}

AgentHub::~AgentHub() {
    running_ = false;
    if (watcher_.joinable()) watcher_.join();
}

void AgentHub::startHeartbeatWatch() {
    running_ = true;
    watcher_ = std::thread([this] { watchLoop(); });
}

void AgentHub::setOnlineNotifier(EventNotifier notifier) {
    notifier_ = std::move(notifier);
}

void AgentHub::setOnAgentMessage(MessageHandler handler) {
    onMessage_ = std::move(handler);
}

void AgentHub::setOnAgentBinary(BinaryHandler handler) {
    onBinary_ = std::move(handler);
}

void AgentHub::onOpen(WsHdl hdl) {

}

void AgentHub::onText(WsHdl hdl, const std::string& text) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        return;
    }
    const std::string type = j.value("type", "");

    if (type == "register") {
        if (j.value("token", std::string{}) != agentToken_) {
            gw_.closeConn(hdl);
            return;
        }
        const std::string hostname = j.value("hostname", std::string{});
        const std::string userName = j.value("userName", std::string{});

        std::string offlineEvent;
        const std::string newAgentId = echonode::common::generateUuid();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto it = agents_.begin(); it != agents_.end();) {
                if (it->second.hostname == hostname && it->second.userName == userName) {
                    offlineEvent = nlohmann::json{{"type", "agent_offline"},
                                                  {"agentId", it->second.agentId},
                                                  {"hostname", hostname}}
                                       .dump();
                    connToId_.erase(it->second.hdl);
                    gw_.closeConn(it->second.hdl);
                    it = agents_.erase(it);
                } else {
                    ++it;
                }
            }

        Agent agent;
            agent.agentId = newAgentId;
            agent.hostname = hostname;
            agent.osName = j.value("osName", std::string{});
            agent.osVersion = j.value("osVersion", std::string{});
            agent.userName = userName;
            agent.arch = j.value("arch", std::string{});
            agent.lastSeen = std::chrono::steady_clock::now();
            agent.hdl = hdl;

            connToId_[hdl] = agent.agentId;
            agents_[agent.agentId] = std::move(agent);

            if (db_) {
                const auto ts = nowSec();
                db_->exec("INSERT OR REPLACE INTO agents VALUES(?,?,?,?,?,?,?,?);",
                          {agent.agentId, hostname, agent.osName, agent.osVersion,
                           userName, agent.arch, ts, ts});
            }
        }

        if (!offlineEvent.empty() && notifier_) notifier_(offlineEvent);
        if (notifier_) {
            notifier_(nlohmann::json{{"type", "agent_online"},
                                     {"agentId", newAgentId},
                                     {"hostname", hostname},
                                     {"userName", userName}}
                          .dump());
        }
        return;
    }

    if (type == "heartbeat") {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = connToId_.find(hdl);
        if (it != connToId_.end()) {
            auto ag = agents_.find(it->second);
            if (ag != agents_.end()) {
                ag->second.lastSeen = std::chrono::steady_clock::now();
                if (db_) {
                    db_->exec("UPDATE agents SET last_seen=? WHERE agent_id=?;",
                              {nowSec(), ag->second.agentId});
                }
            }
        }
        return;
    }

    std::string agentId;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = connToId_.find(hdl);
        if (it == connToId_.end()) return;
        agentId = it->second;
    }
    if (onMessage_) onMessage_(agentId, text);
}

void AgentHub::onBinary(WsHdl hdl, const void* data, size_t len) {
    std::string agentId;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = connToId_.find(hdl);
        if (it == connToId_.end()) return;
        agentId = it->second;
    }
    if (onBinary_) onBinary_(agentId, data, len);
}

void AgentHub::onClose(WsHdl hdl) {
    std::string offlineEvent;
        const std::string newAgentId = echonode::common::generateUuid();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = connToId_.find(hdl);
        if (it == connToId_.end()) return;
        const auto ag = agents_.find(it->second);
        if (ag != agents_.end()) {
            offlineEvent = nlohmann::json{{"type", "agent_offline"},
                                          {"agentId", ag->second.agentId},
                                          {"hostname", ag->second.hostname}}
                               .dump();
            if (db_) {
                db_->exec("UPDATE agents SET last_seen=? WHERE agent_id=?;",
                          {nowSec(), ag->second.agentId});
            }
        }
        agents_.erase(it->second);
        connToId_.erase(it);
    }
    if (!offlineEvent.empty() && notifier_) notifier_(offlineEvent);
}

void AgentHub::watchLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(kWatchIntervalSec));
        if (!running_) break;
        const auto deadline = std::chrono::steady_clock::now() -
                              std::chrono::seconds(kHeartbeatTimeoutSec);
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = agents_.begin(); it != agents_.end();) {
            if (it->second.lastSeen < deadline) {
                gw_.closeConn(it->second.hdl);
                connToId_.erase(it->second.hdl);
                it = agents_.erase(it);
            } else {
                ++it;
            }
        }
    }
}

std::string AgentHub::agentsJson() const {
    std::lock_guard<std::mutex> lk(mtx_);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& [id, a] : agents_) {
        arr.push_back({{"agentId", a.agentId},
                       {"hostname", a.hostname},
                       {"osName", a.osName},
                       {"osVersion", a.osVersion},
                       {"userName", a.userName},
                       {"arch", a.arch},
                       {"online", true}});
    }
    return arr.dump();
}

bool AgentHub::sendTextToAgent(const std::string& agentId, const std::string& text) {
    WsHdl hdl;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = agents_.find(agentId);
        if (it == agents_.end()) return false;
        hdl = it->second.hdl;
    }
    gw_.sendText(hdl, text);
    return true;
}

bool AgentHub::sendBinaryToAgent(const std::string& agentId, const void* data, size_t len) {
    WsHdl hdl;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = agents_.find(agentId);
        if (it == agents_.end()) return false;
        hdl = it->second.hdl;
    }
    gw_.sendBinary(hdl, data, len);
    return true;
}

} // namespace echonode::server
