#include "auth/SessionManager.hpp"
#include "core/ServerConfig.hpp"
#include "db/Database.hpp"
#include "http/HttpApi.hpp"
#include "hub/AgentHub.hpp"
#include "hub/TaskRouter.hpp"
#include "ws/OperatorHub.hpp"
#include "ws/WsGateway.hpp"

#include <iostream>


#ifdef _WIN32

#include <windows.h>
static void initConsoleUtf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
#else
static void initConsoleUtf8() {}
#endif

int main(int argc, char** argv) {
    initConsoleUtf8();
    const std::string configPath = argc > 1 ? argv[1] : "server.json";

    echonode::server::ServerConfig cfg;
    if (!cfg.loadOrInstall(configPath)) return 1;

    echonode::server::Database db("server.db");
    echonode::server::SessionManager sessions;
    echonode::server::WsGateway gateway;
    echonode::server::AgentHub hub(gateway, cfg.agentToken, &db);
    echonode::server::TaskRouter router(hub, db);
    echonode::server::OperatorHub operators(gateway, sessions, hub, router);
    echonode::server::HttpApi api(gateway, hub, sessions, db, "webui",
                                  [&cfg](const std::string& u, const std::string& p) {
                                      return cfg.verifyLogin(u, p);
                                  });

    hub.startHeartbeatWatch();
    try {
        gateway.init(cfg.httpPort, cfg.bindAddr);
    } catch (const std::exception& e) {
        std::cerr << "监听 " << cfg.httpPort << " 失败: " << e.what() << "\n";
        return 1;
    }
    std::cout << "EchoNode server listening on :" << cfg.httpPort << "\n";
    gateway.run(); // 阻塞运行
    return 0;
}
