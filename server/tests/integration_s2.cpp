#include "core/Client.hpp"
#include "core/Config.hpp"
#include "executor/Dispatcher.hpp"
#include "executor/ShellExecutor.hpp"
#include "executor/ShellSessionManager.hpp"
#include "executor/ProcessExecutor.hpp"
#include "platform/PlatformFactory.hpp"
#include "core/ServerConfig.hpp"
#include "hub/AgentHub.hpp"
#include "hub/TaskRouter.hpp"
#include "http/HttpApi.hpp"
#include "ws/OperatorHub.hpp"

#include <asio.hpp>

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

using WsClientEp = websocketpp::client<websocketpp::config::asio_client>;
using WsMessagePtr = websocketpp::config::asio_client::message_type::ptr;

namespace server = echonode::server;
namespace executor = echonode::executor;

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

struct HttpResp {
    int status = 0;
    std::string body;
};

HttpResp httpRequest(const std::string& method, const std::string& path,
                     const std::string& body, const std::string& token, int port) {
    asio::io_context io;
    asio::ip::tcp::socket socket(io);
    socket.connect({asio::ip::make_address("127.0.0.1"), static_cast<uint16_t>(port)});
    std::string req = method + " " + path + " HTTP/1.1\r\nHost: localhost\r\n";
    if (!token.empty()) req += "Authorization: Bearer " + token + "\r\n";
    req += "Content-Type: application/json\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n" + body;
    asio::write(socket, asio::buffer(req));
    std::string raw;
    char buf[4096];
    asio::error_code ec;
    for (;;) {
        size_t n = socket.read_some(asio::buffer(buf), ec);
        if (ec) break;
        raw.append(buf, n);
    }
    HttpResp resp;
    const auto lineEnd = raw.find("\r\n");
    if (lineEnd != std::string::npos) {
        if (raw.find(" 200 ") != std::string::npos) resp.status = 200;
        else if (raw.find(" 401 ") != std::string::npos) resp.status = 401;
        else if (raw.find(" 404 ") != std::string::npos) resp.status = 404;
    }
    const auto pos = raw.find("\r\n\r\n");
    if (pos != std::string::npos) resp.body = raw.substr(pos + 4);
    return resp;
}

}

int main() {
    std::cout << std::unitbuf;

    const std::string cfgPath = "server_s2_test.json";
    auto cfg = server::ServerConfig::forTest("admin", "secret123", "agent-token-s2");
    cfg.save(cfgPath);
    auto loaded = server::ServerConfig::forTest("admin", "secret123", "agent-token-s2");

    const int port = 18941;
    server::Database db("server_s2_test.db");
    server::SessionManager sessions;
    server::WsGateway gateway;
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

    auto login = httpRequest("POST", "/api/login",
                             R"({"username":"admin","password":"secret123"})", "", port);
    check(login.status == 200 && login.body.find("token") != std::string::npos,
          "POST /api/login 签发 token");
    const std::string token = [](const std::string& body) {
        const auto pos = body.find("\"token\":\"");
        if (pos == std::string::npos) return std::string{};
        return body.substr(pos + 9, body.find('"', pos + 9) - pos - 9);
    }(login.body);

    check(httpRequest("GET", "/api/agents", "", "", port).status == 401,
          "未认证访问 /api/agents 被拒(401)");
    check(httpRequest("GET", "/api/agents", "", token, port).status == 200,
          "携带 token 访问 /api/agents 正常");

    WsClientEp opClient;
    opClient.clear_access_channels(websocketpp::log::alevel::all);
    opClient.clear_error_channels(websocketpp::log::elevel::all);
    opClient.init_asio();

    std::mutex mtx;
    bool gotAuthOk = false, gotOnlineEvent = false, gotResult = false;
    std::string resultText, agentId;

    opClient.set_open_handler([&](websocketpp::connection_hdl hdl) {
        websocketpp::lib::error_code e;
        opClient.send(hdl, "{\"type\":\"auth\",\"token\":\"" + token + "\"}",
                      websocketpp::frame::opcode::text, e);
    });
    opClient.set_message_handler([&](websocketpp::connection_hdl hdl, WsMessagePtr msg) {
        if (msg->get_opcode() != websocketpp::frame::opcode::text) return;
        const std::string text = msg->get_payload();
        std::lock_guard<std::mutex> lk(mtx);
        if (text.find("auth_ok") != std::string::npos) gotAuthOk = true;
        if (text.find("agent_online") != std::string::npos) gotOnlineEvent = true;
        if (text.find("task_result") != std::string::npos &&
            text.find("hello") != std::string::npos) {
            gotResult = true;
            resultText = text;
        }
    });
    websocketpp::lib::error_code ec;
    auto opConn = opClient.get_connection("ws://127.0.0.1:" + std::to_string(port) + "/ws", ec);
    opClient.connect(opConn);
    std::thread opThread([&opClient] { opClient.run(); });

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return gotAuthOk; }, 8000),
          "operator WS 认证 (auth_ok)");

    echonode::core::Config ccfg;
    ccfg.url = "ws://127.0.0.1:" + std::to_string(port) + "/agent";
    ccfg.token = "agent-token-s2";
    ccfg.heartbeatMs = 300;
    echonode::core::Client agent(ccfg);
    executor::Dispatcher dispatcher;
    auto mgr = std::make_unique<executor::ShellSessionManager>(
        echonode::platform::createShellSpawn());
    auto* sessionsPtr = mgr.get();
    mgr->setSink([&agent](const std::string& sid, const std::string& data, bool eof) {
        agent.sendText(echonode::protocol::toJson(
                            echonode::protocol::ShellData{sid, data, eof})
                            .dump());
    });
    dispatcher.add(std::make_unique<executor::ShellExecutor>(
        echonode::platform::createShellSpawn(), *mgr));
    dispatcher.add(std::make_unique<executor::ProcessExecutor>(
        echonode::platform::createProcessOps()));
    agent.setTextHook([&sessionsPtr](const nlohmann::json& j) {
        if (j.value("type", std::string{}) != "shell_data") return false;
        sessionsPtr->input(j.value("sessionId", std::string{}),
                           j.value("data", std::string{}));
        return true;
    });
    agent.setTaskHandler([&dispatcher](echonode::protocol::Task task) {
        return dispatcher.dispatch(std::move(task));
    });
    std::thread agentThread([&] { agent.run(); });

    check(waitFor([&] {
              return httpRequest("GET", "/api/agents", "", token, port)
                  .body.find("\"online\":true") != std::string::npos;
          }, 8000),
          "agent 上线");
    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return gotOnlineEvent; }, 8000),
          "operator 收到 agent_online 广播");

    bool taskSent = waitFor([&] {
        auto resp = httpRequest("GET", "/api/agents", "", token, port);
        const auto pos = resp.body.find("\"agentId\":\"");
        if (pos == std::string::npos) return false;
        std::lock_guard<std::mutex> lk(mtx);
        agentId = resp.body.substr(pos + 11, 36);
        websocketpp::lib::error_code e2;
        opClient.send(opConn->get_handle(),
                      std::string("{\"type\":\"task_req\",\"agentId\":\"") + agentId +
                          R"(","action":"shell_exec","payload":{"command":"echo hello"}})",
                      websocketpp::frame::opcode::text, e2);
        return true;
    }, 8000);
    check(taskSent, "task_req 已下发");

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return gotResult; }, 10000),
          "operator 收到 task_result(含命令输出)");

    check(waitFor([&] {
              auto resp = httpRequest("GET", "/api/tasks", "", token, port);
              return resp.body.find("shell_exec") != std::string::npos &&
                     resp.body.find("done") != std::string::npos;
          }, 5000),
          "任务历史落库 (GET /api/tasks 状态 done)");

    agent.stop();
    agentThread.join();
    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return resultText.find("agent_offline") != std::string::npos; }, 8000) ||
              waitFor([&] {
                  auto resp = httpRequest("GET", "/api/agents", "", token, port);
                  return resp.body.find("\"online\":true") == std::string::npos;
              }, 8000),
          "agent 下线后列表为空");

    opClient.stop();
    opThread.join();
    gateway.stop();
    serverThread.join();

    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}