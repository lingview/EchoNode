#include "core/Client.hpp"
#include "core/Config.hpp"
#include "core/ServerConfig.hpp"
#include "db/Database.hpp"
#include "hub/AgentHub.hpp"
#include "hub/TaskRouter.hpp"
#include "http/HttpApi.hpp"
#include "util/Uuid.hpp"
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

    auto loaded = server::ServerConfig::forTest("admin", "secret123", "agent-token-s6");
    loaded.save("server_s6_test.json");

    const int port = 18945;
    server::Database db("server_s6_test.db");
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
    ccfg.token = "agent-token-s6";
    echonode::core::Client agent(ccfg);
    agent.setTaskHandler([&agent](echonode::protocol::Task task) -> echonode::protocol::TaskResult {
        if (task.action == "remote_start") {
            const std::string tid = task.taskId;
            std::thread([&agent, tid] {

                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                const std::string idBytes = echonode::common::uuidToBytes(tid);
                for (uint32_t seq = 0; seq < 3; ++seq) {
                    std::string frame = idBytes;
                    frame += static_cast<char>((seq >> 24) & 0xFF);
                    frame += static_cast<char>((seq >> 16) & 0xFF);
                    frame += static_cast<char>((seq >> 8) & 0xFF);
                    frame += static_cast<char>(seq & 0xFF);
                    frame += static_cast<char>(0x06);
                    frame += "tilepayload";
                    agent.sendBinary(frame.data(), frame.size());
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }).detach();
            return {task.taskId, true, R"({"w":1280,"h":720})", {}};
        }
        return {task.taskId, false, {}, "unsupported action"};
    });
    std::thread agentThread([&] { agent.run(); });

    WsClientEp opClient;
    opClient.clear_access_channels(websocketpp::log::alevel::all);
    opClient.clear_error_channels(websocketpp::log::elevel::all);
    opClient.init_asio();

    std::mutex mtx;
    std::string agentId;
    bool gotResult = false;
    int deskFrames = 0;

    const std::string opToken = sessions.create("admin");
    opClient.set_open_handler([&](websocketpp::connection_hdl hdl) {
        websocketpp::lib::error_code e;
        opClient.send(hdl, nlohmann::json{{"type", "auth"}, {"token", opToken}}.dump(),
                      websocketpp::frame::opcode::text, e);
    });
    opClient.set_message_handler([&](websocketpp::connection_hdl hdl, WsMessagePtr msg) {
        if (msg->get_opcode() == websocketpp::frame::opcode::binary) {
            const auto& p = msg->get_payload();
            if (p.size() >= 21 && static_cast<uint8_t>(p[20]) == 0x06) {
                std::lock_guard<std::mutex> lk(mtx);
                ++deskFrames;
            }
            return;
        }
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(msg->get_payload());
        } catch (const std::exception&) {
            return;
        }
        std::lock_guard<std::mutex> lk(mtx);
        const std::string type = j.value("type", std::string{});
        if (type == "agent_online" && agentId.empty()) {
            agentId = j.value("agentId", std::string{});
            websocketpp::lib::error_code e;
            opClient.send(hdl, nlohmann::json{{"type", "task_req"},
                                              {"agentId", agentId},
                                              {"action", "remote_start"},
                                              {"payload", {{"fps", 30}}}}
                              .dump(), websocketpp::frame::opcode::text, e);
            return;
        }
        if (type == "task_result" && j.value("action", "") == "remote_start")
            gotResult = true;
    });
    websocketpp::lib::error_code ec;
    auto opConn = opClient.get_connection("ws://127.0.0.1:" + std::to_string(port) + "/ws", ec);
    opClient.connect(opConn);
    std::thread opThread([&opClient] { opClient.run(); });

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return !agentId.empty(); }, 8000),
          "agent 上线广播");
    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return gotResult; }, 8000),
          "remote_start 回传 task_result(w/h)");

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return deskFrames >= 3; }, 8000),
          "task_result 之后桌面 0x06 帧仍路由到 operator（streams_ 保活）");

    agent.stop();
    agentThread.join();
    opClient.stop();
    opThread.join();
    gateway.stop();
    serverThread.join();

    std::remove("server_s6_test.json");
    std::remove("server_s6_test.db");
    std::remove("server_s6_test.db-shm");
    std::remove("server_s6_test.db-wal");

    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}