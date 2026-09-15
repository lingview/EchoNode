#include "Client.hpp"

#include "../net/WsClient.hpp"
#include "../platform/PlatformFactory.hpp"
#include "protocol/to_server.hpp"
#include "protocol/from_server.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace echonode::core {

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

struct Client::Impl {
    Config cfg;
    net::WsClient ws;
    std::atomic<bool> running{false};
    std::mutex wakeMtx;
    std::condition_variable wakeCv;
    std::thread heartbeat;

    Client::TaskHandler taskHandler = [](protocol::Task task) {
        return protocol::TaskResult{task.taskId, false, {}, "action not implemented: " + task.action};
    };

    Client::TextHook textHook;
    Client::BinaryHook binaryHook;

    explicit Impl(Config c)
        : cfg(std::move(c)),
          ws(cfg.url, cfg.backoffBaseMs, cfg.backoffCapMs) {}

    void onOpen() {
        auto host = platform::createSystemInfo()->getHostInfo();
        ws.sendText(protocol::toJson(protocol::Register{
                        host.hostname, host.osName, host.osVersion,
                        host.userName, host.arch, cfg.token})
                        .dump());
    }

    void onText(const std::string& text) {
        nlohmann::json j;
        try {
            j = nlohmann::json::parse(text);
        } catch (const std::exception&) {
            return;
        }

        if (textHook && textHook(j)) return;

        protocol::Task task;
        if (protocol::fromJson(j, task)) {
            auto result = taskHandler(std::move(task));
            if (!result.taskId.empty()) ws.sendText(protocol::toJson(result).dump());
        }
    }

    void heartbeatLoop() {
        while (running) {
            for (int done = 0; done < cfg.heartbeatMs && running; done += 50) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!running) break;
            ws.sendText(protocol::toJson(protocol::Heartbeat{nowMs()}).dump());
        }
    }
};

Client::Client(Config cfg) : impl_(new Impl(std::move(cfg))) {
    impl_->ws.setHandlers([this] { impl_->onOpen(); },
                          [this](const std::string& text) { impl_->onText(text); },
                          nullptr);
    impl_->ws.setBinaryHandler([this](const void* data, size_t len) {
        if (impl_->binaryHook && impl_->binaryHook(data, len)) return;
    });
}

Client::~Client() { stop(); }

void Client::setTaskHandler(TaskHandler handler) {
    impl_->taskHandler = std::move(handler);
}

void Client::setTextHook(TextHook hook) {
    impl_->textHook = std::move(hook);
}

void Client::setBinaryHook(BinaryHook hook) {
    impl_->binaryHook = std::move(hook);
}

bool Client::sendBinary(const void* data, size_t len) {
    return impl_->ws.sendBinary(data, len);
}

bool Client::sendText(const std::string& text) {
    return impl_->ws.sendText(text);
}

void Client::run() {
    auto& p = *impl_;
    bool expected = false;
    if (!p.running.compare_exchange_strong(expected, true)) return;

    p.ws.start();
    p.heartbeat = std::thread([&p] { p.heartbeatLoop(); });

    // 阻塞至stop()
    std::unique_lock<std::mutex> lk(p.wakeMtx);
    p.wakeCv.wait(lk, [&p] { return !p.running.load(); });
}

void Client::stop() {
    auto& p = *impl_;
    if (!p.running.exchange(false)) return;
    p.wakeCv.notify_all();
    p.ws.stop();
    if (p.heartbeat.joinable()) p.heartbeat.join();
}

} // namespace echonode::core
