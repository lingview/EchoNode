#pragma once
#include "../auth/SessionManager.hpp"
#include "../db/Database.hpp"
#include "../hub/TaskRouter.hpp"

#include <map>
#include <string>

namespace echonode::server {

class OperatorHub : public TaskRouter::OperatorSink {
public:
    OperatorHub(WsGateway& gw, SessionManager& sessions, AgentHub& hub, TaskRouter& router);

    void broadcast(const std::string& text);

    void sendToOperator(WsHdl hdl, const std::string& text) override;
    void sendBinaryToOperator(WsHdl hdl, const void* data, size_t len) override;

private:
    void onOpen(WsHdl hdl);
    void onText(WsHdl hdl, const std::string& text);
    void onBinary(WsHdl hdl, const void* data, size_t len);
    void onClose(WsHdl hdl);

    WsGateway& gw_;
    SessionManager& sessions_;
    AgentHub& hub_;
    TaskRouter& router_;
    std::mutex mtx_;
    std::map<WsHdl, std::string, HdlLess> authed_;
};

} // namespace echonode::server
