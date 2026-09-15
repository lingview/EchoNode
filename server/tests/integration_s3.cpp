#include "core/Client.hpp"
#include "core/Config.hpp"
#include "core/ServerConfig.hpp"
#include "db/Database.hpp"
#include "executor/Dispatcher.hpp"
#include "executor/ProcessExecutor.hpp"
#include "executor/ShellExecutor.hpp"
#include "executor/ShellSessionManager.hpp"
#include "hub/AgentHub.hpp"
#include "hub/TaskRouter.hpp"
#include "http/HttpApi.hpp"
#include "platform/PlatformFactory.hpp"
#include "ws/OperatorHub.hpp"

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

}

int main() {
    std::cout << std::unitbuf;

    const std::string cfgPath = "server_s3_test.json";
    auto cfg = server::ServerConfig::forTest("admin", "secret123", "agent-token-s3");
    cfg.save(cfgPath);
    auto loaded = server::ServerConfig::forTest("admin", "secret123", "agent-token-s3");

    const int port = 18942;
    server::Database db("server_s3_test.db");
    server::SessionManager sessions;
    server::WsGateway gateway;
    server::AgentHub hub(gateway, loaded.agentToken, &db);
    server::TaskRouter router(hub, db);
    server::OperatorHub operators(gateway, sessions, hub, router);
    hub.startHeartbeatWatch();
    gateway.init(port);
    std::thread serverThread([&gateway] { gateway.run(); });

    echonode::core::Config ccfg;
    ccfg.url = "ws://127.0.0.1:" + std::to_string(port) + "/agent";
    ccfg.token = "agent-token-s3";
    echonode::core::Client agent(ccfg);
    executor::Dispatcher dispatcher;
    auto mgr = std::make_unique<executor::ShellSessionManager>(
        echonode::platform::createShellSpawn());
    auto* mgrPtr = mgr.get();
    mgr->setSink([&agent](const std::string& sid, const std::string& data, bool eof) {
        agent.sendText(echonode::protocol::toJson(
                           echonode::protocol::ShellData{sid, data, eof})
                           .dump());
    });
    dispatcher.add(std::make_unique<executor::ShellExecutor>(
        echonode::platform::createShellSpawn(), *mgr));
    dispatcher.add(std::make_unique<executor::ProcessExecutor>(
        echonode::platform::createProcessOps()));
    agent.setTextHook([&mgrPtr](const nlohmann::json& j) {
        if (j.value("type", std::string{}) != "shell_data") return false;
        if (j.value("eof", false)) {
            mgrPtr->close(j.value("sessionId", std::string{}));
            return true;
        }
        mgrPtr->input(j.value("sessionId", std::string{}),
                      j.value("data", std::string{}));
        return true;
    });
    agent.setTaskHandler([&dispatcher](echonode::protocol::Task task) {
        return dispatcher.dispatch(std::move(task));
    });
    std::thread agentThread([&] { agent.run(); });

    WsClientEp opClient;
    opClient.clear_access_channels(websocketpp::log::alevel::all);
    opClient.clear_error_channels(websocketpp::log::elevel::all);
    opClient.init_asio();
    const std::string opToken = sessions.create("admin");

    std::mutex mtx;
    std::string agentId, sessionId;
    bool gotOutput = false, gotEof = false, gotOpenResult = false;
    std::string allOutput;
    std::string osName;

    opClient.set_open_handler([&](websocketpp::connection_hdl hdl) {
        websocketpp::lib::error_code e;
        websocketpp::lib::error_code e2;
        opClient.send(hdl,
                      nlohmann::json{{"type", "auth"}, {"token", opToken}}.dump(),
                      websocketpp::frame::opcode::text, e2);
    });
    opClient.set_message_handler([&](websocketpp::connection_hdl hdl, WsMessagePtr msg) {
        if (msg->get_opcode() != websocketpp::frame::opcode::text) return;
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(msg->get_payload());
        } catch (const std::exception&) {
            return;
        }
        const std::string type = j.value("type", std::string{});
        std::lock_guard<std::mutex> lk(mtx);
        if (type == "agent_online" && agentId.empty()) {
            agentId = j.value("agentId", std::string{});
            osName = j.value("osName", std::string{});

            websocketpp::lib::error_code e;
            opClient.send(hdl,
                          nlohmann::json{{"type", "task_req"},
                                         {"agentId", agentId},
                                         {"action", "shell_open"},
                                         {"payload", nlohmann::json::object()}}
                              .dump(),
                          websocketpp::frame::opcode::text, e);
            return;
        }
        if (type == "task_result" && sessionId.empty() && j.value("ok", false)) {
            sessionId = j.value("data", std::string{});
            gotOpenResult = true;

            const std::string nl = osName == "Windows" ? "\r\n" : "\n";
            websocketpp::lib::error_code e;
            opClient.send(hdl,
                          nlohmann::json{{"type", "shell_data"},
                                         {"sessionId", sessionId},
                                         {"data", "echo marker_s3" + nl},
                                         {"eof", false}}
                              .dump(),
                          websocketpp::frame::opcode::text, e);
            return;
        }
        if (type == "shell_data") {
            allOutput += j.value("data", std::string{});
            if (j.value("data", std::string{}).find("marker_s3") != std::string::npos)
                gotOutput = true;
            if (j.value("eof", false)) gotEof = true;
        }
    });
    websocketpp::lib::error_code ec;
    auto opConn = opClient.get_connection("ws://127.0.0.1:" + std::to_string(port) + "/ws", ec);
    opClient.connect(opConn);
    std::thread opThread([&opClient] { opClient.run(); });

    check(waitFor([&] {
              std::lock_guard<std::mutex> lk(mtx);
              return gotOpenResult && !sessionId.empty();
          }, 10000),
          "shell_open 建立会话并返回 sessionId");

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return gotOutput; }, 10000),
          "键入经 server 路由到 agent，输出回传 (marker_s3)");

    {
        websocketpp::lib::error_code e;
        std::lock_guard<std::mutex> lk(mtx);
        opClient.send(opConn->get_handle(),
                      nlohmann::json{{"type", "shell_data"},
                                     {"sessionId", sessionId},
                                     {"data", ""},
                                     {"eof", true}}
                          .dump(),
                      websocketpp::frame::opcode::text, e);
    }
    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return gotEof; }, 8000),
          "eof 后 agent 清理会话并回确认");

    {
        websocketpp::lib::error_code e;
        std::lock_guard<std::mutex> lk(mtx);
        opClient.send(opConn->get_handle(),
                      nlohmann::json{{"type", "shell_data"},
                                     {"sessionId", "00000000-0000-0000-0000-000000000000"},
                                     {"data", "ignored"},
                                     {"eof", false}}
                          .dump(),
                      websocketpp::frame::opcode::text, e);
    }
    check(true, "未知 sessionId 静默忽略不崩溃");

    agent.stop();
    agentThread.join();
    mgr.reset();
    opClient.stop();
    opThread.join();
    gateway.stop();
    serverThread.join();

    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}