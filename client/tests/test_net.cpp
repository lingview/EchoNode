#include "WsClient.hpp"

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using WsServer = websocketpp::server<websocketpp::config::asio>;
using WsMessagePtr = websocketpp::config::asio::message_type::ptr;

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

    WsServer server;
    server.clear_access_channels(websocketpp::log::alevel::all);
    server.clear_error_channels(websocketpp::log::elevel::all);
    server.init_asio();

    server.set_message_handler([&server](websocketpp::connection_hdl hdl, WsMessagePtr msg) {
        std::string payload = msg->get_payload();
        websocketpp::lib::error_code ec;
        server.send(hdl, payload, websocketpp::frame::opcode::text, ec);
        if (payload == "close-me") {
            server.close(hdl, websocketpp::close::status::normal, "bye");
        }
    });

    try {
        server.listen(18925);
    } catch (const std::exception& e) {
        std::cout << "[FAIL] echo 服务监听 18925 失败: " << e.what() << "\n";
        return 1;
    }
    server.start_accept();
    std::thread serverThread([&server] { server.run(); });

    echonode::net::WsClient client("ws://127.0.0.1:18925", 500, 60000);

    std::mutex mtx;
    int opens = 0, closes = 0;
    std::vector<std::string> received;

    client.setHandlers(
        [&] { std::lock_guard<std::mutex> lk(mtx); ++opens; },
        [&](const std::string& m) { std::lock_guard<std::mutex> lk(mtx); received.push_back(m); },
        [&] { std::lock_guard<std::mutex> lk(mtx); ++closes; });
    client.start();

    auto lastReceived = [&] {
        std::lock_guard<std::mutex> lk(mtx);
        return received.empty() ? std::string() : received.back();
    };

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return opens >= 1; }, 5000),
          "连接建立 (onOpen)");
    client.sendText("ping-1");
    check(waitFor([&] { return lastReceived() == "ping-1"; }, 5000), "发送 + echo 接收");

    const int opensBefore = [&] { std::lock_guard<std::mutex> lk(mtx); return opens; }();
    const int closesBefore = [&] { std::lock_guard<std::mutex> lk(mtx); return closes; }();
    client.sendText("close-me");
    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return closes > closesBefore; }, 5000),
          "断线检测 (onClose)");
    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return opens > opensBefore; }, 8000),
          "退避重连 (再次 onOpen)");

    client.sendText("ping-2");
    check(waitFor([&] { return lastReceived() == "ping-2"; }, 5000), "重连后收发正常");

    client.stop();
    server.stop();
    serverThread.join();

    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}