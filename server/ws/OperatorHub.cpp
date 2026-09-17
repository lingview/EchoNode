#include "OperatorHub.hpp"

namespace echonode::server {

OperatorHub::OperatorHub(WsGateway& gw, SessionManager& sessions, AgentHub& hub,
                         TaskRouter& router)
    : gw_(gw), sessions_(sessions), hub_(hub), router_(router) {
    WsRoute route;
    route.onOpen = [this](WsHdl h) { onOpen(h); };
    route.onText = [this](WsHdl h, const std::string& t) { onText(h, t); };
    route.onBinary = [this](WsHdl h, const void* d, size_t l) { onBinary(h, d, l); };
    route.onClose = [this](WsHdl h) { onClose(h); };
    gw_.addRoute("/ws", std::move(route));

    hub.setOnlineNotifier([this](const std::string& event) { broadcast(event); });
    router_.setOperatorSink(this);
}

void OperatorHub::onOpen(WsHdl hdl) {
}

void OperatorHub::onText(WsHdl hdl, const std::string& text) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        return;
    }
    const std::string type = j.value("type", "");

    if (type == "auth") {
        const std::string token = j.value("token", std::string{});
        if (!sessions_.validate(token)) {
            gw_.closeConn(hdl);
            return;
        }
        std::lock_guard<std::mutex> lk(mtx_);
        authed_[hdl] = token;
        gw_.sendText(hdl, "{\"type\":\"auth_ok\"}");
        return;
    }
    if (type == "shell_data" || type == "shell_resize") {
        bool authedSd = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            authedSd = authed_.count(hdl) != 0;
        }
        if (authedSd) router_.onOperatorShellData(hdl, j);
        return;
    }
    if (type == "file_ack") {
        bool authedFa = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            authedFa = authed_.count(hdl) != 0;
        }
        if (authedFa) router_.onOperatorFileAck(hdl, j);
        return;
    }
    if (type == "desk_stat") {
        bool authedDs = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            authedDs = authed_.count(hdl) != 0;
        }
        if (authedDs) router_.onOperatorDeskStat(hdl, j);
        return;
    }
    if (type == "task_req") {
        bool authed = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            authed = authed_.count(hdl) != 0;
        }
        if (!authed) return;
        const std::string agentId = j.value("agentId", std::string{});
        router_.submitTask(hdl, agentId, j);
        return;
    }
}

void OperatorHub::onBinary(WsHdl hdl, const void* data, size_t len) {
    bool authed = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        authed = authed_.count(hdl) != 0;
    }
    if (authed) router_.onOperatorBinary(hdl, data, len);
}

void OperatorHub::onClose(WsHdl hdl) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        authed_.erase(hdl);
    }
    router_.operatorGone(hdl);
}

void OperatorHub::broadcast(const std::string& text) {
    std::vector<WsHdl> targets;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& [hdl, user] : authed_) targets.push_back(hdl);
    }
    for (auto& hdl : targets) gw_.sendText(hdl, text);
}

void OperatorHub::sendToOperator(WsHdl hdl, const std::string& text) {
    gw_.sendText(hdl, text);
}

void OperatorHub::sendBinaryToOperator(WsHdl hdl, const void* data, size_t len) {
    gw_.sendBinary(hdl, data, len);
}

} // namespace echonode::server
