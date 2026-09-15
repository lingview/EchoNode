#include "core/Client.hpp"
#include "core/Config.hpp"
#include "executor/Dispatcher.hpp"
#include "executor/ShellExecutor.hpp"
#include "executor/ShellSessionManager.hpp"
#include "platform/PlatformFactory.hpp"
#include "protocol/to_server.hpp"
#include "util/Uuid.hpp"

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

using WsServer = websocketpp::server<websocketpp::config::asio>;
using WsMessagePtr = websocketpp::config::asio::message_type::ptr;
using nlohmann::json;

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

namespace executor = echonode::executor;

int main() {
;
    WsServer server;
    server.clear_access_channels(websocketpp::log::alevel::all);
    server.clear_error_channels(websocketpp::log::elevel::all);
    server.init_asio();

    std::mutex mtx;
    std::string osName;
    websocketpp::connection_hdl clientHdl;
    bool execOk = false;
    std::string execData;
    std::string sessionId;

    std::vector<std::pair<std::string, bool>> shellOut;

    auto sendToClient = [&server, &mtx, &clientHdl](const json& j) {
        websocketpp::lib::asio::post(server.get_io_service(), [&server, &mtx, &clientHdl, j] {
            websocketpp::lib::error_code ec;
            std::lock_guard<std::mutex> lk(mtx);
            server.send(clientHdl, j.dump(), websocketpp::frame::opcode::text, ec);
        });
    };

    server.set_open_handler([&](websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lk(mtx);
        clientHdl = hdl;
    });

    server.set_message_handler([&](websocketpp::connection_hdl, WsMessagePtr msg) {
        json j;
        try {
            j = json::parse(msg->get_payload());
        } catch (const std::exception&) {
            return;
        }
        std::lock_guard<std::mutex> lk(mtx);
        const std::string type = j.value("type", "");

        if (type == "register") {
            osName = j.value("osName", "");
            sendToClient({{"type", "task"},
                          {"taskId", echonode::common::generateUuid()},
                          {"action", "shell_exec"},
                          {"payload", {{"command", "echo hello"}}}});
        } else if (type == "task_result") {
            if (!j.value("ok", true))
                std::cerr << "[txdbg] error=" << j.value("error", std::string{})
                          << " data=" << j.value("data", std::string{}) << std::endl;
            const bool ok = j.value("ok", false);
            const std::string data = j.value("data", "");
            if (data.find("hello") != std::string::npos && ok) {
                execOk = true;
                execData = data;

                sendToClient({{"type", "task"},
                              {"taskId", echonode::common::generateUuid()},
                              {"action", "shell_open"},
                              {"payload", json::object()}});
            } else if (ok && !sessionId.empty()) {

            } else if (ok) {

                sessionId = data;
                const std::string nl = osName == "Windows" ? "\r\n" : "\n";
                sendToClient({{"type", "shell_data"},
                              {"sessionId", sessionId},
                              {"data", "echo marker123" + nl},
                              {"eof", false}});
            }
        } else if (type == "shell_data") {
            const bool eof = j.value("eof", false);
            shellOut.emplace_back(j.value("data", ""), eof);
            if (!eof && j.value("data", "").find("marker123") != std::string::npos) {

                sendToClient({{"type", "task"},
                              {"taskId", echonode::common::generateUuid()},
                              {"action", "shell_close"},
                              {"payload", {{"sessionId", sessionId}}}});
            }
        }
    });

    try {
        server.listen(18927);
    } catch (const std::exception& e) {
        std::cout << "[FAIL] 假 Server 监听 18927 失败: " << e.what() << "\n";
        return 1;
    }
    server.start_accept();
    std::thread serverThread([&server] { server.run(); });

    echonode::core::Config cfg;
    cfg.url = "ws://127.0.0.1:18927";
    cfg.token = "test-token";
    echonode::core::Client client(cfg);

    executor::Dispatcher dispatcher;
    auto sessions = std::make_unique<echonode::executor::ShellSessionManager>(
        echonode::platform::createShellSpawn());
    auto* sessionsPtr = sessions.get();
    sessions->setSink([&client](const std::string& sessionId, const std::string& data, bool eof) {
        client.sendText(echonode::protocol::toJson(
                            echonode::protocol::ShellData{sessionId, data, eof})
                            .dump());
    });
    dispatcher.add(std::make_unique<echonode::executor::ShellExecutor>(
        echonode::platform::createShellSpawn(), *sessions));
    client.setTextHook([&sessionsPtr](const nlohmann::json& j) {
        if (j.value("type", std::string{}) != "shell_data") return false;
        sessionsPtr->input(j.value("sessionId", std::string{}),
                           j.value("data", std::string{}));
        return true;
    });
    client.setTaskHandler([&dispatcher](echonode::protocol::Task task) {
        return dispatcher.dispatch(std::move(task));
    });

    std::thread clientThread([&] { client.run(); });

    check(waitFor([&] {
              std::lock_guard<std::mutex> lk(mtx);
              return execOk;
          }, 8000),
          "shell_exec 一次性命令 (echo hello)");

    check(waitFor([&] {
              std::lock_guard<std::mutex> lk(mtx);
              return !sessionId.empty();
          }, 8000),
          "shell_open 返回 sessionId");

    check(waitFor([&] {
              std::lock_guard<std::mutex> lk(mtx);
              for (const auto& [data, eof] : shellOut) {
                  if (!eof && data.find("marker123") != std::string::npos) return true;
              }
              return false;
          }, 8000),
          "交互会话: 写入 stdin 并读到输出 (marker123)");

    check(waitFor([&] {
              std::lock_guard<std::mutex> lk(mtx);
              for (const auto& [data, eof] : shellOut) {
                  if (eof) return true;
              }
              return false;
          }, 8000),
          "shell_close 后收到 eof");

    client.stop();
    clientThread.join();
    sessions.reset();
    server.stop();
    serverThread.join();

    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}