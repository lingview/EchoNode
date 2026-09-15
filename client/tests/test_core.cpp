#include "core/Client.hpp"
#include "core/Config.hpp"
#include "util/Uuid.hpp"

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>

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

int main() {
    std::cout << std::unitbuf;
    WsServer server;
    server.clear_access_channels(websocketpp::log::alevel::all);
    server.clear_error_channels(websocketpp::log::elevel::all);
    server.init_asio();

    std::mutex mtx;
    int registers = 0, heartbeats = 0;
    std::string firstRegister;
    std::string lastResultTaskId, lastResultError;
    bool resultOk = true;
    bool closeRequested = false;
    websocketpp::connection_hdl clientHdl;

    server.set_open_handler([&](websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lk(mtx);
        clientHdl = hdl;
    });

    server.set_message_handler([&](websocketpp::connection_hdl hdl, WsMessagePtr msg) {
        json j;
        try {
            j = json::parse(msg->get_payload());
        } catch (const std::exception&) {
            return;
        }
        std::lock_guard<std::mutex> lk(mtx);
        const std::string type = j.value("type", "");

        if (type == "register") {
            if (registers == 0) {
                firstRegister = j.dump();

                json task = {{"type", "task"},
                             {"taskId", echonode::common::generateUuid()},
                             {"action", "no_such_action"},
                             {"payload", json::object()}};
                websocketpp::lib::error_code ec;
                server.send(hdl, task.dump(), websocketpp::frame::opcode::text, ec);
            }
            ++registers;
        } else if (type == "task_result") {
            lastResultTaskId = j.value("taskId", "");
            resultOk = j.value("ok", true);
            lastResultError = j.value("error", "");
            closeRequested = true;
        } else if (type == "heartbeat") {
            ++heartbeats;
        }
    });

    std::thread closer([&] {
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            websocketpp::connection_hdl hdl;
            {
                std::lock_guard<std::mutex> lk(mtx);
                if (!closeRequested) continue;
                closeRequested = false;
                hdl = clientHdl;
            }

             websocketpp::lib::asio::post(server.get_io_service(), [hdl, &server] {
                websocketpp::lib::error_code ec2;
                server.close(hdl, websocketpp::close::status::normal, "test", ec2);
            });
        }
    });
    closer.detach();

    try {
        server.listen(18926);
    } catch (const std::exception& e) {
        std::cout << "[FAIL] 假 Server 监听 18926 失败: " << e.what() << "\n";
        return 1;
    }
    server.start_accept();
    std::thread serverThread([&server] { server.run(); });

    echonode::core::Config cfg;
    cfg.url = "ws://127.0.0.1:18926";
    cfg.token = "test-token";
    cfg.heartbeatMs = 300;
    cfg.backoffBaseMs = 500;
    echonode::core::Client client(cfg);
    std::thread clientThread([&] { client.run(); });

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return registers >= 1; }, 5000),
          "首次注册 (register)");

    bool fieldsOk = false;
    {
        std::lock_guard<std::mutex> lk(mtx);
        if (!firstRegister.empty()) {
            json j = json::parse(firstRegister);
            fieldsOk = j.value("token", "") == "test-token" &&
                       !j.value("hostname", "").empty() &&
                       !j.value("osName", "").empty() &&
                       !j.value("arch", "").empty();
        }
    }
    check(fieldsOk, "register 字段完整 (token/hostname/os/arch)");

    check(waitFor([&] {
              std::lock_guard<std::mutex> lk(mtx);
              return !lastResultTaskId.empty() && !resultOk &&
                     lastResultError.find("not implemented") != std::string::npos;
          }, 5000),
          "任务派发与 task_result (未知 action 被拒绝)");

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return heartbeats >= 1; }, 3000),
          "心跳上报");

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return registers >= 2; }, 8000),
          "断线重连后重新注册");

    client.stop();
    clientThread.join();
    server.stop();
    serverThread.join();

    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}