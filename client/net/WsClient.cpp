#include "WsClient.hpp"

#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_no_tls_client.hpp>

#include <chrono>
#include <mutex>
#include <thread>

using WsEndpoint = websocketpp::client<websocketpp::config::asio_client>;
using WsMessagePtr = websocketpp::config::asio_client::message_type::ptr;
using WsHdl = websocketpp::connection_hdl;

namespace echonode::net {

struct WsClient::Impl {
    std::string url;
    int backoffBaseMs = 1000;
    int backoffCapMs = 60000;
    int backoffMs = 1000;

    WsClient::OpenHandler onOpen;
    WsClient::TextHandler onText;
    WsClient::BinaryHandler onBinary;
    WsClient::CloseHandler onClose;

    WsEndpoint endpoint;
    WsHdl currentHdl;
    std::mutex hdlMutex;
    bool connected = false;    // 持锁访问
    bool openedThisRun = false; // 本次是否真正连上过

    std::atomic<bool> running{false};
    std::thread worker;

    // 分片睡眠，便于 stop 及时打断退避等待
    void sleepBackoff() {
        for (int done = 0; done < backoffMs && running; done += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    void runLoop() {
        backoffMs = backoffBaseMs;
        while (running) {
            websocketpp::lib::error_code ec;
            auto conn = endpoint.get_connection(url, ec);
            if (!ec) {
                openedThisRun = false;
                endpoint.get_io_service().restart(); // stop() 后的 start() 复用需要 restart
                endpoint.connect(conn);
                endpoint.run(); // 阻塞至连接断开（io_service 无事件即返回）

                if (openedThisRun) {
                    openedThisRun = false;
                    if (onClose) onClose();
                }
            }
            if (!running) break;

            sleepBackoff();
            backoffMs = std::min(backoffMs * 2, backoffCapMs);
        }
    }

    void dropConnection() {
        std::lock_guard<std::mutex> lk(hdlMutex);
        currentHdl.reset();
        connected = false;
    }
};

WsClient::WsClient(std::string url, int backoffBaseMs, int backoffCapMs)
    : impl_(new Impl) {
    impl_->url = std::move(url);
    impl_->backoffBaseMs = backoffBaseMs;
    impl_->backoffCapMs = backoffCapMs;

    auto& p = *impl_;
    p.endpoint.clear_access_channels(websocketpp::log::alevel::all);
    p.endpoint.clear_error_channels(websocketpp::log::elevel::all);
    p.endpoint.init_asio();

    p.endpoint.set_open_handler([&p](WsHdl hdl) {
        {
            std::lock_guard<std::mutex> lk(p.hdlMutex);
            p.currentHdl = hdl;
            p.connected = true;
            p.openedThisRun = true;
            p.backoffMs = p.backoffBaseMs; // 连上即重置退避
        }
        // 锁外回调：onOpen 内会 sendText 再次加锁，持锁回调会自死锁
        if (p.onOpen) p.onOpen();
    });
    p.endpoint.set_message_handler([&p](WsHdl, WsMessagePtr msg) {
        if (msg->get_opcode() == websocketpp::frame::opcode::binary) {
            if (p.onBinary) {
                const auto& payload = msg->get_payload();
                p.onBinary(payload.data(), payload.size());
            }
            return;
        }
        if (p.onText) p.onText(msg->get_payload());
    });
    auto drop = [&p](WsHdl) { p.dropConnection(); };
    p.endpoint.set_close_handler(drop);
    p.endpoint.set_fail_handler(drop);
}

WsClient::~WsClient() { stop(); }

void WsClient::setHandlers(OpenHandler onOpen, TextHandler onText, CloseHandler onClose) {
    impl_->onOpen = std::move(onOpen);
    impl_->onText = std::move(onText);
    impl_->onClose = std::move(onClose);
}

void WsClient::setBinaryHandler(BinaryHandler onBinary) {
    impl_->onBinary = std::move(onBinary);
}

void WsClient::start() {
    auto& p = *impl_;
    bool expected = false;
    if (!p.running.compare_exchange_strong(expected, true)) return;
    p.worker = std::thread([&p] { p.runLoop(); });
}

void WsClient::stop() {
    auto& p = *impl_;
    if (!p.running.exchange(false)) return;

    // 先优雅关闭当前连接，服务端才能及时感知下线
    bool needClose = false;
    WsHdl hdl;
    {
        std::lock_guard<std::mutex> lk(p.hdlMutex);
        needClose = p.connected;
        hdl = p.currentHdl;
    }
    if (needClose) {
        websocketpp::lib::asio::post(p.endpoint.get_io_service(), [&p, hdl] {
            websocketpp::lib::error_code ec;
            p.endpoint.close(hdl, websocketpp::close::status::normal, "stop", ec);
        });
        // 给 close frame 一个发送窗口再终止 io
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    p.endpoint.get_io_service().stop();
    if (p.worker.joinable()) p.worker.join();
}

bool WsClient::sendBinary(const void* data, size_t len) {
    auto& p = *impl_;
    if (!p.running) return false;

    WsHdl hdl;
    {
        std::lock_guard<std::mutex> lk(p.hdlMutex);
        if (!p.connected) return false;
        hdl = p.currentHdl;
    }

    // 复制成共享缓冲投递回网络线程，避免调用方生命周期问题
    auto payload = std::make_shared<std::vector<uint8_t>>(
        static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
    websocketpp::lib::asio::post(p.endpoint.get_io_service(), [&p, hdl, payload] {
        websocketpp::lib::error_code ec;
        p.endpoint.send(hdl, payload->data(), payload->size(),
                        websocketpp::frame::opcode::binary, ec);
    });
    return true;
}

bool WsClient::sendText(const std::string& text) {
    auto& p = *impl_;
    if (!p.running) return false;

    WsHdl hdl;
    {
        std::lock_guard<std::mutex> lk(p.hdlMutex);
        if (!p.connected) return false;
        hdl = p.currentHdl;
    }

    // 投递回网络线程发送，endpoint 不允许跨线程并发 send
    auto payload = std::make_shared<std::string>(text);
    websocketpp::lib::asio::post(p.endpoint.get_io_service(), [&p, hdl, payload] {
        websocketpp::lib::error_code ec;
        p.endpoint.send(hdl, *payload, websocketpp::frame::opcode::text, ec);
    });
    return true;
}

} // namespace echonode::net
