#include "core/Client.hpp"
#include "core/Config.hpp"
#include "auth/SessionManager.hpp"
#include "core/ServerConfig.hpp"
#include "db/Database.hpp"
#include "hub/AgentHub.hpp"
#include "hub/TaskRouter.hpp"
#include "ws/OperatorHub.hpp"
#include "http/HttpApi.hpp"
#include "ws/WsGateway.hpp"

#include <asio.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace server = echonode::server;

namespace {

int gFailed = 0;

void check(bool ok, const std::string& name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
    if (!ok) ++gFailed;
}

template <typename Pred>
bool waitFor(Pred pred, int timeoutMs) {
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 25) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return pred();
}

std::string httpGet(const std::string& path, int port, const std::string& token = {}) {
    asio::io_context io;
    asio::ip::tcp::socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"),
                    static_cast<uint16_t>(port)});
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n";
    if (!token.empty()) req += "Authorization: Bearer " + token + "\r\n";
    req += "Connection: close\r\n\r\n";
    asio::write(socket, asio::buffer(req));
    std::string raw;
    char buf[4096];
    asio::error_code ec;
    for (;;) {
        size_t n = socket.read_some(asio::buffer(buf), ec);
        if (ec) break;
        raw.append(buf, n);
    }
    const auto pos = raw.find("\r\n\r\n");
    return pos == std::string::npos ? "" : raw.substr(pos + 4);
}

}

int main() {
    std::cout << std::unitbuf;

    const std::string cfgPath = "server_s1_test.json";
    auto cfg = server::ServerConfig::forTest("admin", "secret123", "agent-token-s1");
    if (!cfg.save(cfgPath)) {
        std::cout << "[FAIL] 无法写入测试配置\n";
        return 1;
    }

    server::ServerConfig loaded;
    if (!loaded.loadOrInstall(cfgPath)) {
        std::cout << "[FAIL] 无法读取测试配置\n";
        return 1;
    }
    check(loaded.verifyLogin("admin", "secret123"), "登录校验(正确凭据)");
    check(!loaded.verifyLogin("admin", "wrong"), "登录校验(错误密码拒绝)");
    check(!loaded.verifyLogin("root", "secret123"), "登录校验(错误用户拒绝)");

    const int port = 18940;
    server::WsGateway gateway;
    server::Database db("server_s1_test.db");
    server::SessionManager sessions;
    server::AgentHub hub(gateway, loaded.agentToken, &db);
    server::TaskRouter router(hub, db);
    server::OperatorHub operators(gateway, sessions, hub, router);
    server::HttpApi api(gateway, hub, sessions, db, "webui",
                        [&loaded](const std::string& u, const std::string& p) {
                            return loaded.verifyLogin(u, p);
                        });

    hub.startHeartbeatWatch();
    gateway.init(port);

    std::thread serverThread([&gateway] { gateway.run(); });

    std::string token;
    {
        asio::io_context io;
        asio::ip::tcp::socket s(io);
        s.connect({asio::ip::make_address("127.0.0.1"), static_cast<uint16_t>(port)});
        const std::string body = R"({"username":"admin","password":"secret123"})";
        std::string req = "POST /api/login HTTP/1.1\r\nHost: localhost\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: " + std::to_string(body.size()) + "\r\n"
                          "Connection: close\r\n\r\n" + body;
        asio::write(s, asio::buffer(req));
        std::string raw;
        char buf[4096];
        asio::error_code ec;
        for (;;) {
            size_t n = s.read_some(asio::buffer(buf), ec);
            if (ec) break;
            raw.append(buf, n);
        }
        const auto pos = raw.find("\"token\":\"");
        if (pos != std::string::npos) token = raw.substr(pos + 9, raw.find("\"", pos + 9) - pos - 9);
    }

    echonode::core::Config ccfg;
    ccfg.url = "ws://127.0.0.1:" + std::to_string(port) + "/agent";
    ccfg.token = "agent-token-s1";
    ccfg.heartbeatMs = 300;
    echonode::core::Client client(ccfg);
    std::thread clientThread([&] { client.run(); });

    check(waitFor([&] {
              auto body = httpGet("/api/agents", port, token);
              return body.find("\"hostname\"") != std::string::npos &&
                     body.find("\"online\":true") != std::string::npos;
          }, 8000),
          "agent 上线并出现在 /api/agents");

    std::this_thread::sleep_for(std::chrono::seconds(2));
    {
        auto body = httpGet("/api/agents", port, token);
        check(body.find("\"online\":true") != std::string::npos, "心跳保持在线");
    }

    client.stop();
    clientThread.join();
    check(waitFor([&] {
              auto body = httpGet("/api/agents", port, token);
              return body.find("\"hostname\"") == std::string::npos;
          }, 8000),
          "agent 下线后从列表消失");

    {
        auto body = httpGet("/definitely_missing.html", port);
        std::cerr << "[s1] 404 body: [" << body << "]" << std::endl;
        check(body.find("not found") != std::string::npos, "静态文件 404 兜底");
    }

    gateway.stop();
    serverThread.join();

    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}