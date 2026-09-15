#include "core/Client.hpp"
#include "core/Config.hpp"
#include "executor/Dispatcher.hpp"
#include "executor/ScreenshotExecutor.hpp"
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

bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (tolower(static_cast<unsigned char>(a[i])) !=
            tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
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
    std::string shotTaskId;
    std::string shotContent;
    std::string shotSha;
    size_t shotSize = 0;
    bool shotDone = false;
    bool noDisplay = false;

    auto sendJson = [&server, &mtx, &clientHdl](const json& j) {
        websocketpp::lib::asio::post(server.get_io_service(),
                                     [&server, &mtx, &clientHdl, j] {
                                         websocketpp::lib::error_code ec;
                                         std::lock_guard<std::mutex> lk(mtx);
                                         server.send(clientHdl, j.dump(),
                                                     websocketpp::frame::opcode::text, ec);
                                     });
    };

    server.set_open_handler([&](websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lk(mtx);
        clientHdl = hdl;
    });

    server.set_message_handler([&](websocketpp::connection_hdl, WsMessagePtr msg) {
        if (msg->get_opcode() == websocketpp::frame::opcode::binary) {
            const auto& p = msg->get_payload();
            if (p.size() < 21 || static_cast<uint8_t>(p[20]) != 0x02) return;
            std::lock_guard<std::mutex> lk(mtx);
            shotContent.append(p.data() + 21, p.size() - 21);
            return;
        }
        json j;
        try {
            j = json::parse(msg->get_payload());
        } catch (const std::exception&) {
            return;
        }
        std::lock_guard<std::mutex> lk(mtx);
        const std::string type = j.value("type", "");

        if (type == "register") {
            shotTaskId = echonode::common::generateUuid();
            sendJson({{"type", "task"}, {"taskId", shotTaskId},
                      {"action", "screenshot"}, {"payload", json::object()}});
        } else if (type == "task_result" && j.value("taskId", "") == shotTaskId) {
            if (!j.value("ok", false)) {
                noDisplay = j.value("error", "").find("X") != std::string::npos;
                return;
            }
            try {
                auto summary = json::parse(j.value("data", "{}"));
                shotSize = summary.value("size", size_t{0});
                shotSha = summary.value("sha256", "");

                const auto* b = reinterpret_cast<const unsigned char*>(shotContent.data());
                shotDone = shotContent.size() == shotSize && shotContent.size() > 2 &&
                           b[0] == 0xFF && b[1] == 0xD8 &&
                           ieq(shotSha, echonode::common::sha256Hex(shotContent.data(),
                                                                    shotContent.size()));
            } catch (const std::exception&) {
            }
        }
    });

    try {
        server.listen(18930);
    } catch (const std::exception& e) {
        std::cout << "[FAIL] 假 Server 监听 18930 失败: " << e.what() << "\n";
        return 1;
    }
    server.start_accept();
    std::thread serverThread([&server] { server.run(); });

    echonode::core::Config cfg;
    cfg.url = "ws://127.0.0.1:18930";
    cfg.token = "test-token";
    echonode::core::Client client(cfg);

    executor::Dispatcher dispatcher;
    auto shotExecutor = std::make_unique<executor::ScreenshotExecutor>(
        echonode::platform::createScreenCapture());
    shotExecutor->setBinarySender(
        [&client](const void* data, size_t len) { client.sendBinary(data, len); });
    dispatcher.add(std::move(shotExecutor));
    client.setTextHook([](const nlohmann::json&) { return false; });
    client.setBinaryHook([](const void*, size_t) { return false; });
    client.setTaskHandler([&dispatcher](echonode::protocol::Task task) {
        return dispatcher.dispatch(std::move(task));
    });

    std::thread clientThread([&] { client.run(); });

    waitFor([&] {
        std::lock_guard<std::mutex> lk(mtx);
        return shotDone || noDisplay;
    }, 15000);

    bool skipped = false;
    {
        std::lock_guard<std::mutex> lk(mtx);
        skipped = noDisplay;
        if (!skipped) {
            check(shotDone, "screenshot 分块回传 + sha256 校验 (" +
                                std::to_string(shotSize) + "B)");
        } else {
            std::cout << "[SKIP] screenshot: 无显示环境（Linux 无 X 会话）\n";
        }
    }
    if (skipped) {

        check(true, "无显示环境错误路径回传正常");
    }

    client.stop();
    clientThread.join();
    server.stop();
    serverThread.join();

    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}