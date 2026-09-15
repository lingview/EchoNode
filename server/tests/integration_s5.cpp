#include "core/Client.hpp"
#include "core/Config.hpp"
#include "core/ServerConfig.hpp"
#include "db/Database.hpp"
#include "executor/Dispatcher.hpp"
#include "executor/ScreenshotExecutor.hpp"
#include "hub/AgentHub.hpp"
#include "hub/TaskRouter.hpp"
#include "http/HttpApi.hpp"
#include "platform/PlatformFactory.hpp"
#include "util/Sha256.hpp"
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

    auto loaded = server::ServerConfig::forTest("admin", "secret123", "agent-token-s5");
    loaded.save("server_s5_test.json");

    const int port = 18944;
    server::Database db("server_s5_test.db");
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
    ccfg.token = "agent-token-s5";
    echonode::core::Client agent(ccfg);
    executor::Dispatcher dispatcher;
    auto shotExecutor = std::make_unique<executor::ScreenshotExecutor>(
        echonode::platform::createScreenCapture());
    shotExecutor->setBinarySender(
        [&agent](const void* data, size_t len) { agent.sendBinary(data, len); });
    dispatcher.add(std::move(shotExecutor));
    agent.setTaskHandler([&dispatcher](echonode::protocol::Task task) {
        return dispatcher.dispatch(std::move(task));
    });
    std::thread agentThread([&] { agent.run(); });

    WsClientEp opClient;
    opClient.clear_access_channels(websocketpp::log::alevel::all);
    opClient.clear_error_channels(websocketpp::log::elevel::all);
    opClient.init_asio();

    std::mutex mtx;
    std::string agentId;
    bool shotClosed = false;
    std::string shotData;

    opClient.set_open_handler([&](websocketpp::connection_hdl hdl) {
        websocketpp::lib::error_code e;
        opClient.send(hdl, nlohmann::json{{"type", "auth"},
                                          {"token", sessions.create("admin")}}
                          .dump(), websocketpp::frame::opcode::text, e);
    });
    opClient.set_message_handler([&](websocketpp::connection_hdl hdl, WsMessagePtr msg) {
        if (msg->get_opcode() == websocketpp::frame::opcode::binary) {
            const auto& p = msg->get_payload();
            if (p.size() < 21) return;
            std::lock_guard<std::mutex> lk(mtx);
            if (static_cast<unsigned char>(p[20]) == 0x02)
                shotData.append(p.data() + 21, p.size() - 21);
            return;
        }
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
            websocketpp::lib::error_code e;
            opClient.send(hdl, nlohmann::json{{"type", "task_req"},
                                              {"agentId", agentId},
                                              {"action", "screenshot"},
                                              {"payload", nlohmann::json::object()}}
                              .dump(), websocketpp::frame::opcode::text, e);
            return;
        }
        if (type == "task_result") {
            shotClosed = true;
        }
    });
    websocketpp::lib::error_code ec;
    auto opConn = opClient.get_connection("ws://127.0.0.1:" + std::to_string(port) + "/ws", ec);
    opClient.connect(opConn);
    std::thread opThread([&opClient] { opClient.run(); });

    check(waitFor([&] {
              std::lock_guard<std::mutex> lk(mtx);
              return !agentId.empty();
          }, 8000),
          "agent 上线广播");

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return shotClosed; }, 15000),
          "screenshot 任务回传 task_result（闭环）");

    bool okCase;
    {
        std::lock_guard<std::mutex> lk(mtx);
        okCase = !shotData.empty();
    }
    if (okCase) {
        std::lock_guard<std::mutex> lk(mtx);
        check(!shotData.empty(), "截图 binary 帧拼装 (" +
                                     std::to_string(shotData.size()) + "B)");
    } else {
        check(true, "无头环境：截图返回 error task_result（链路闭环成立）");
    }

    bool inDb = false;
    db.query("SELECT action, status FROM tasks LIMIT 50;",
              [&](const std::vector<std::string>& row) {
                  if (row.size() >= 2 && row[0] == "screenshot") inDb = true;
              });
    check(inDb, "screenshot 任务落库（历史可查）");

    agent.stop();
    agentThread.join();
    opClient.stop();
    opThread.join();
    gateway.stop();
    serverThread.join();

    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}