#include "WsClient.hpp"

#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/config/asio_client.hpp>

#include <asio/ssl.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace echonode::net {

struct WsBackend {
    std::string url;
    int backoffBaseMs = 1000;
    int backoffCapMs = 60000;
    int backoffMs = 1000;

    WsClient::OpenHandler onOpen;
    WsClient::TextHandler onText;
    WsClient::BinaryHandler onBinary;
    WsClient::CloseHandler onClose;

    websocketpp::connection_hdl currentHdl;
    std::mutex hdlMutex;
    bool connected = false;
    bool openedThisRun = false;

    std::atomic<bool> running{false};
    std::thread worker;

    virtual ~WsBackend() = default;
    virtual void initEndpoint() = 0;
    virtual void runLoop() = 0;
    virtual bool sendText(const std::string& text) = 0;
    virtual bool sendBinary(const void* data, size_t len) = 0;
    virtual void teardown() = 0;

    void sleepBackoff() {
        for (int done = 0; done < backoffMs && running; done += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    void dropConnection() {
        std::lock_guard<std::mutex> lk(hdlMutex);
        currentHdl.reset();
        connected = false;
    }
    bool connectedNow() {
        std::lock_guard<std::mutex> lk(hdlMutex);
        return connected;
    }
};

template <class Cfg, bool IsTls>
struct WsBackendT final : WsBackend {
    using Endpoint = websocketpp::client<Cfg>;
    using MsgPtr = typename Cfg::message_type::ptr;
    Endpoint endpoint;

    void initEndpoint() override {
        endpoint.clear_access_channels(websocketpp::log::alevel::all);
        endpoint.clear_error_channels(websocketpp::log::elevel::all);
        endpoint.init_asio();
        if constexpr (IsTls) {
            endpoint.set_tls_init_handler([](websocketpp::connection_hdl) {
                auto ctx = std::make_shared<asio::ssl::context>(
                    asio::ssl::context::tls_client);
                ctx->set_verify_mode(asio::ssl::verify_none);
                return ctx;
            });
        }

        WsBackend* self = this;
        endpoint.set_open_handler([self](websocketpp::connection_hdl hdl) {
            {
                std::lock_guard<std::mutex> lk(self->hdlMutex);
                self->currentHdl = hdl;
                self->connected = true;
                self->openedThisRun = true;
                self->backoffMs = self->backoffBaseMs;
            }
            if (self->onOpen) self->onOpen();
        });
        endpoint.set_message_handler([self](websocketpp::connection_hdl, MsgPtr msg) {
            if (msg->get_opcode() == websocketpp::frame::opcode::binary) {
                if (self->onBinary) {
                    const auto& payload = msg->get_payload();
                    self->onBinary(payload.data(), payload.size());
                }
                return;
            }
            if (self->onText) self->onText(msg->get_payload());
        });
        auto drop = [self](websocketpp::connection_hdl) { self->dropConnection(); };
        endpoint.set_close_handler(drop);
        endpoint.set_fail_handler(drop);
    }

    void runLoop() override {
        backoffMs = backoffBaseMs;
        while (running) {
            websocketpp::lib::error_code ec;
            auto conn = endpoint.get_connection(url, ec);
            if (!ec) {
                openedThisRun = false;
                endpoint.get_io_service().restart();
                endpoint.connect(conn);
                endpoint.run();

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

    bool sendText(const std::string& text) override {
        if (!running) return false;
        websocketpp::connection_hdl hdl;
        {
            std::lock_guard<std::mutex> lk(hdlMutex);
            if (!connected) return false;
            hdl = currentHdl;
        }
        auto payload = std::make_shared<std::string>(text);
        websocketpp::lib::asio::post(endpoint.get_io_service(),
                                     [this, hdl, payload] {
                                         websocketpp::lib::error_code ec;
                                         endpoint.send(hdl, *payload,
                                                       websocketpp::frame::opcode::text, ec);
                                     });
        return true;
    }

    bool sendBinary(const void* data, size_t len) override {
        if (!running) return false;
        websocketpp::connection_hdl hdl;
        {
            std::lock_guard<std::mutex> lk(hdlMutex);
            if (!connected) return false;
            hdl = currentHdl;
        }
        auto payload = std::make_shared<std::vector<uint8_t>>(
            static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
        websocketpp::lib::asio::post(endpoint.get_io_service(),
                                     [this, hdl, payload] {
                                         websocketpp::lib::error_code ec;
                                         endpoint.send(hdl, payload->data(), payload->size(),
                                                       websocketpp::frame::opcode::binary, ec);
                                     });
        return true;
    }

    void teardown() override {
        bool needClose = false;
        websocketpp::connection_hdl hdl;
        {
            std::lock_guard<std::mutex> lk(hdlMutex);
            needClose = connected;
            hdl = currentHdl;
        }
        if (needClose) {
            websocketpp::lib::asio::post(endpoint.get_io_service(), [this, hdl] {
                websocketpp::lib::error_code ec;
                endpoint.close(hdl, websocketpp::close::status::normal, "stop", ec);
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        endpoint.get_io_service().stop();
    }
};

WsClient::WsClient(std::string url, int backoffBaseMs, int backoffCapMs) {
    const bool tls = (url.rfind("wss://", 0) == 0);
    if (tls)
        impl_ = std::make_unique<WsBackendT<websocketpp::config::asio_tls_client, true>>();
    else
        impl_ = std::make_unique<WsBackendT<websocketpp::config::asio_client, false>>();

    impl_->url = std::move(url);
    impl_->backoffBaseMs = backoffBaseMs;
    impl_->backoffCapMs = backoffCapMs;
    impl_->backoffMs = backoffBaseMs;
    impl_->initEndpoint();
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
    bool expected = false;
    if (!impl_->running.compare_exchange_strong(expected, true)) return;
    WsBackend* b = impl_.get();
    b->worker = std::thread([b] { b->runLoop(); });
}

void WsClient::stop() {
    if (!impl_->running.exchange(false)) return;
    impl_->teardown();
    if (impl_->worker.joinable()) impl_->worker.join();
}

bool WsClient::sendText(const std::string& text) { return impl_->sendText(text); }

bool WsClient::sendBinary(const void* data, size_t len) { return impl_->sendBinary(data, len); }

} // namespace echonode::net
