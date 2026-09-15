#include "core/Client.hpp"
#include "core/Config.hpp"
#include "core/ServerConfig.hpp"
#include "db/Database.hpp"
#include "executor/Dispatcher.hpp"
#include "executor/FileExecutor.hpp"
#include "executor/ProcessExecutor.hpp"
#include "executor/ShellSessionManager.hpp"
#include "hub/AgentHub.hpp"
#include "hub/TaskRouter.hpp"
#include "http/HttpApi.hpp"
#include "platform/PlatformFactory.hpp"
#include "util/Sha256.hpp"
#include "util/Uuid.hpp"
#include "ws/OperatorHub.hpp"

#include <websocketpp/config/asio_no_tls_client.hpp>
#include <websocketpp/client.hpp>

#include <chrono>
#include <cstring>
#include <fstream>
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
constexpr size_t CHUNK = 32768;
constexpr size_t WINDOW = 4;

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
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower(static_cast<unsigned char>(a[i])) !=
            tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

std::string bytesToUuid(const unsigned char* b) {
    static const char* hexd = "0123456789abcdef";
    std::string h;
    for (int i = 0; i < 16; ++i) {
        h += hexd[b[i] >> 4];
        h += hexd[b[i] & 0xF];
    }
    return h.substr(0,8)+"-"+h.substr(8,4)+"-"+h.substr(12,4)+"-"+h.substr(16,4)+"-"+h.substr(20,12);
}

std::string buildFrame(const std::string& idBytes, uint32_t seq, size_t offset,
                       const char* data, size_t len) {
    std::string f = idBytes;
    f += static_cast<char>((seq >> 24) & 0xFF);
    f += static_cast<char>((seq >> 16) & 0xFF);
    f += static_cast<char>((seq >> 8) & 0xFF);
    f += static_cast<char>(seq & 0xFF);
    f += static_cast<char>(0x01);
    for (int i = 7; i >= 0; --i)
        f += static_cast<char>((offset >> (i * 8)) & 0xFF);
    f.append(data, len);
    return f;
}

}

int main() {
    std::cout << std::unitbuf;

    const std::string srcPath = "s4_src.bin";
    const std::string dstPath = "s4_dst.bin";
    std::string content;
    for (int i = 0; i < 200000; ++i) content += static_cast<char>((i * 17 + 3) & 0xFF);
    {
        std::ofstream f(srcPath, std::ios::binary | std::ios::trunc);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    const std::string contentSha = echonode::common::sha256Hex(content.data(), content.size());
    const size_t N = content.size();

    const std::string cfgPath = "server_s4_test.json";
    auto loaded = server::ServerConfig::forTest("admin", "secret123", "agent-token-s4");
    loaded.save(cfgPath);

    const int port = 18943;
    server::Database db("server_s4_test.db");
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
    ccfg.token = "agent-token-s4";
    echonode::core::Client agent(ccfg);
    executor::Dispatcher dispatcher;
    auto fileExecutor = std::make_unique<executor::FileExecutor>();
    fileExecutor->setBinarySender(
        [&agent](const void* data, size_t len) { agent.sendBinary(data, len); });
    fileExecutor->setResultSender(
        [&agent](const echonode::protocol::TaskResult& r) {
            agent.sendText(echonode::protocol::toJson(r).dump());
        });
    fileExecutor->setTextSender(
        [&agent](const std::string& text) { agent.sendText(text); });
    auto* filePtr = fileExecutor.get();
    dispatcher.add(std::move(fileExecutor));
    dispatcher.add(std::make_unique<executor::ProcessExecutor>(
        echonode::platform::createProcessOps()));
    agent.setBinaryHook([filePtr](const void* data, size_t len) {
        return filePtr->onBinary(data, len);
    });

    agent.setTextHook([filePtr](const nlohmann::json& j) {
        if (j.value("type", std::string{}) == "file_ack") {
            filePtr->onFileAck(j.value("taskId", std::string{}),
                               j.value("ackedOffset", size_t{0}));
            return true;
        }
        return false;
    });
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

    std::string dlContent, dlSha, dlTaskId;
    size_t dlReceived = 0;
    bool dlDone = false;

    std::string upTaskId, upIdBytes;
    size_t upNext = 0, upAcked = 0;
    uint32_t upSeq = 0;
    bool upReady = false, upOk = false;

    const std::string opToken = sessions.create("admin");
    opClient.set_open_handler([&](websocketpp::connection_hdl hdl) {
        websocketpp::lib::error_code e;
        opClient.send(hdl, nlohmann::json{{"type", "auth"}, {"token", opToken}}.dump(),
                      websocketpp::frame::opcode::text, e);
    });

    auto pumpUpload = [&](websocketpp::connection_hdl hdl) {
        while (upNext < N && (upNext - upAcked) < WINDOW * CHUNK) {
            const size_t take = std::min(CHUNK, N - upNext);
            const std::string frame =
                buildFrame(upIdBytes, upSeq++, upNext, content.data() + upNext, take);
            websocketpp::lib::error_code e;
            opClient.send(hdl, frame.data(), frame.size(),
                          websocketpp::frame::opcode::binary, e);
            upNext += take;
        }
    };

    opClient.set_message_handler([&](websocketpp::connection_hdl hdl, WsMessagePtr msg) {
        if (msg->get_opcode() == websocketpp::frame::opcode::binary) {
            const auto& p = msg->get_payload();
            if (p.size() < 29 || static_cast<uint8_t>(p[20]) != 0x01) return;
            std::lock_guard<std::mutex> lk(mtx);

            if (dlTaskId.empty())
                dlTaskId = bytesToUuid(reinterpret_cast<const unsigned char*>(p.data()));
            size_t offset = 0;
            for (int i = 0; i < 8; ++i)
                offset = (offset << 8) | static_cast<uint8_t>(p[21 + i]);
            const size_t len = p.size() - 29;
            if (dlContent.size() < offset + len) dlContent.resize(offset + len, '\0');
            std::memcpy(&dlContent[offset], p.data() + 29, len);
            if (offset == dlReceived) dlReceived = offset + len;

            websocketpp::lib::error_code e;
            opClient.send(hdl, nlohmann::json{{"type", "file_ack"},
                                              {"taskId", dlTaskId},
                                              {"ackedOffset", dlReceived}}
                              .dump(), websocketpp::frame::opcode::text, e);
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
                                              {"action", "file_download"},
                                              {"payload", {{"path", srcPath},
                                                           {"resumeFrom", 0},
                                                           {"chunkSize", CHUNK},
                                                           {"windowSize", WINDOW}}}}
                              .dump(), websocketpp::frame::opcode::text, e);
            return;
        }
        if (type == "file_ack") {

            const size_t acked = j.value("ackedOffset", size_t{0});
            if (acked > upAcked) upAcked = acked;
            if (upReady) pumpUpload(hdl);
            return;
        }
        if (type != "task_result") return;
        const std::string action = j.value("action", std::string{});
        const std::string data = j.value("data", "");

        if (action == "file_download" && data.find("\"sha256\"") != std::string::npos) {
            try {
                auto summary = nlohmann::json::parse(data);
                dlSha = summary.value("sha256", "");
                dlDone = summary.value("size", size_t{0}) == N && dlContent == content &&
                         ieq(dlSha, contentSha);
            } catch (const std::exception&) {}

            websocketpp::lib::error_code e;
            opClient.send(hdl, nlohmann::json{{"type", "task_req"},
                                              {"agentId", agentId},
                                              {"action", "file_upload"},
                                              {"payload", {{"path", dstPath},
                                                           {"size", N},
                                                           {"sha256", contentSha},
                                                           {"resumeFrom", 0}}}}
                              .dump(), websocketpp::frame::opcode::text, e);
            return;
        }

        if (action == "file_upload" && !upReady) {
            try {
                auto r = nlohmann::json::parse(data);
                if (r.value("ready", false)) {
                    upReady = true;
                    upTaskId = j.value("taskId", std::string{});
                    upIdBytes = echonode::common::uuidToBytes(upTaskId);
                    upNext = upAcked = r.value("resumeFrom", size_t{0});
                    pumpUpload(hdl);
                    return;
                }
            } catch (const std::exception&) {}
        }

        if (action == "file_upload" && upReady && data == "ok" && j.value("ok", false))
            upOk = true;
    });
    websocketpp::lib::error_code ec;
    auto opConn = opClient.get_connection("ws://127.0.0.1:" + std::to_string(port) + "/ws", ec);
    opClient.connect(opConn);
    std::thread opThread([&opClient] { opClient.run(); });

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return !agentId.empty(); }, 8000),
          "agent 上线广播");

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return dlDone; }, 20000),
          "file_download 经 server 路由 + 窗口 ack + sha256 校验 (" +
              std::to_string(dlContent.size()) + "B)");

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return upReady; }, 8000),
          "file_upload 返回 ready(JSON)");

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return upOk; }, 20000),
          "file_upload 窗口 ack 收满校验后回 ok");

    bool dstMatches = false;
    {
        std::ifstream f(dstPath, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        dstMatches = got == content;
    }
    check(dstMatches, "上传落地文件逐字节一致");

    agent.stop();
    agentThread.join();
    opClient.stop();
    opThread.join();
    gateway.stop();
    serverThread.join();

    std::remove(srcPath.c_str());
    std::remove(dstPath.c_str());

    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}