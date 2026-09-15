// HTTP 路由：登录认证、REST API（agents/tasks）+ webui 静态文件托管
#pragma once
#include "../auth/SessionManager.hpp"
#include "../db/Database.hpp"
#include "../hub/AgentHub.hpp"

#include <string>

namespace echonode::server {

class HttpApi {
public:
    HttpApi(WsGateway& gw, AgentHub& hub, SessionManager& sessions, Database& db,
            std::string webRoot,
            std::function<bool(const std::string&, const std::string&)> verifyLogin);
    void handle(WsHdl hdl);

private:
    void respondJson(WsHdl hdl, const std::string& body, int status);
    void respondFile(WsHdl hdl, const std::string& path);
    static std::string mimeType(const std::string& path);
    bool authorized(WsHdl hdl) const;

    WsGateway& gw_;
    AgentHub& hub_;
    SessionManager& sessions_;
    Database& db_;
    std::function<bool(const std::string&, const std::string&)> verifyLogin_;
    std::string webRoot_;
};

}
