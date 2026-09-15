#include "core/Client.hpp"
#include "core/Config.hpp"
#include "executor/Dispatcher.hpp"
#include "executor/ProcessExecutor.hpp"
#include "platform/PlatformFactory.hpp"
#include "protocol/to_server.hpp"
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

    WsServer server;
    server.clear_access_channels(websocketpp::log::alevel::all);
    server.clear_error_channels(websocketpp::log::elevel::all);
    server.init_asio();

    std::mutex mtx;
    websocketpp::connection_hdl clientHdl;
    bool listOk = false;
    size_t listSize = 0;
    bool killRejected = false;
    std::string killError;

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
            sendToClient({{"type", "task"},
                          {"taskId", echonode::common::generateUuid()},
                          {"action", "ps_list"},
                          {"payload", json::object()}});
        } else if (type == "task_result") {
            const bool ok = j.value("ok", false);
            const std::string data = j.value("data", "");
            if (data.find("\"pid\"") != std::string::npos) {

                try {
                    auto arr = json::parse(data);
                    if (ok && arr.is_array() && !arr.empty() &&
                        arr[0].contains("pid") && arr[0].contains("name")) {
                        listOk = true;
                        listSize = arr.size();
                    }
                } catch (const std::exception&) {
                }

                sendToClient({{"type", "task"},
                              {"taskId", echonode::common::generateUuid()},
                              {"action", "ps_kill"},
                              {"payload", {{"pid", 2147483647}}}});
            } else if (j.value("error", "").find("kill failed") != std::string::npos) {
                killRejected = ok == false;
                killError = j.value("error", "");
            }
        }
    });

    try {
        server.listen(18928);
    } catch (const std::exception& e) {
        std::cout << "[FAIL] 假 Server 监听 18928 失败: " << e.what() << "\n";
        return 1;
    }
    server.start_accept();
    std::thread serverThread([&server] { server.run(); });

    echonode::core::Config cfg;
    cfg.url = "ws://127.0.0.1:18928";
    cfg.token = "test-token";
    echonode::core::Client client(cfg);

    executor::Dispatcher dispatcher;
    dispatcher.add(std::make_unique<executor::ProcessExecutor>(
        echonode::platform::createProcessOps()));
    client.setTaskHandler([&dispatcher](echonode::protocol::Task task) {
        return dispatcher.dispatch(std::move(task));
    });

    std::thread clientThread([&] { client.run(); });

    check(waitFor([&] {
              std::lock_guard<std::mutex> lk(mtx);
              return listOk;
          }, 8000),
          "ps_list 返回非空进程数组 (size=" + std::to_string(listSize) + ")");

    check(waitFor([&] {
              std::lock_guard<std::mutex> lk(mtx);
              return killRejected;
          }, 8000),
          "ps_kill 拒绝无效 pid: " + killError);

    client.stop();
    clientThread.join();
    server.stop();
    serverThread.join();

    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}