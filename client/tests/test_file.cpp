#include "core/Client.hpp"
#include "core/Config.hpp"
#include "executor/Dispatcher.hpp"
#include "executor/FileExecutor.hpp"
#include "executor/ProcessExecutor.hpp"
#include "platform/PlatformFactory.hpp"
#include "protocol/to_server.hpp"
#include "util/Sha256.hpp"
#include "util/Uuid.hpp"

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using WsServer = websocketpp::server<websocketpp::config::asio>;
using WsMessagePtr = websocketpp::config::asio::message_type::ptr;
using nlohmann::json;

namespace executor = echonode::executor;

namespace {

int gFailed = 0;
constexpr size_t CHUNK = 262144;
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

size_t parseOffset(const std::string& p) {
    size_t off = 0;
    for (int i = 0; i < 8; ++i)
        off = (off << 8) | static_cast<uint8_t>(p[21 + i]);
    return off;
}

std::string tempPath(const std::string& name) {
    auto os = echonode::platform::createSystemInfo()->getHostInfo().osName;
    const char* t = getenv("TEMP");
    return os == "Windows"
               ? std::string(t ? t : ".") + "\\" + name
               : std::string("/tmp/") + name;
}

}

int main() {
    std::cout << std::unitbuf;

    const size_t N = 2 * 1024 * 1024;
    std::string content(N, '\0');
    for (size_t i = 0; i < N; ++i) content[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    const std::string contentSha = echonode::common::sha256Hex(content.data(), content.size());

    const std::string srcPath = tempPath("echonode_ft_src.bin");
    const std::string dstPath = tempPath("echonode_ft_dst.bin");
    const std::string dstResume = tempPath("echonode_ft_dst_resume.bin");
    {
        std::ofstream f(srcPath, std::ios::binary | std::ios::trunc);
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    WsServer server;
    server.clear_access_channels(websocketpp::log::alevel::all);
    server.clear_error_channels(websocketpp::log::elevel::all);
    server.init_asio();

    std::mutex mtx;
    websocketpp::connection_hdl clientHdl;

    enum Stage { DL, UP, DL_RESUME, UP_RESUME, DONE } stage = DL;

    std::string dlBuf;
    size_t dlReceived = 0;
    size_t dlResumeFrom = 0;
    std::string dlSha, dlTaskId;
    size_t dlSize = 0;
    bool dlDone = false, dlResumeDone = false;

    std::string upTaskId, upIdBytes;
    size_t upNext = 0, upAcked = 0, upResumeFrom = 0;
    uint32_t upSeq = 0;
    bool upReady = false, upOk = false, upResumeOk = false;
    std::string upTargetPath;

    auto postBinary = [&](const std::string& frame) {
        websocketpp::lib::asio::post(server.get_io_service(), [&server, &mtx, &clientHdl, frame] {
            websocketpp::lib::error_code ec;
            std::lock_guard<std::mutex> lk(mtx);
            server.send(clientHdl, frame.data(), frame.size(),
                        websocketpp::frame::opcode::binary, ec);
        });
    };
    auto postText = [&](const json& j) {
        websocketpp::lib::asio::post(server.get_io_service(), [&server, &mtx, &clientHdl, j] {
            websocketpp::lib::error_code ec;
            std::lock_guard<std::mutex> lk(mtx);
            server.send(clientHdl, j.dump(), websocketpp::frame::opcode::text, ec);
        });
    };

    auto pumpUpload = [&] {
        while (upNext < N && (upNext - upAcked) < WINDOW * CHUNK) {
            const size_t take = std::min(CHUNK, N - upNext);
            postBinary(buildFrame(upIdBytes, upSeq++, upNext, content.data() + upNext, take));
            upNext += take;
        }
    };

    server.set_open_handler([&](websocketpp::connection_hdl hdl) {
        std::lock_guard<std::mutex> lk(mtx);
        clientHdl = hdl;
    });

    server.set_message_handler([&](websocketpp::connection_hdl, WsMessagePtr msg) {
        if (msg->get_opcode() == websocketpp::frame::opcode::binary) {
            const auto& p = msg->get_payload();
            if (p.size() < 29 || static_cast<uint8_t>(p[20]) != 0x01) return;
            std::lock_guard<std::mutex> lk(mtx);
            const size_t offset = parseOffset(p);
            const size_t len = p.size() - 29;

            if (dlBuf.size() < offset + len) dlBuf.resize(offset + len, '\0');
            std::memcpy(&dlBuf[offset], p.data() + 29, len);
            if (offset == dlReceived) dlReceived = offset + len;

            if (!dlTaskId.empty())
                postText({{"type", "file_ack"}, {"taskId", dlTaskId},
                          {"ackedOffset", dlReceived}});
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

            dlTaskId = echonode::common::generateUuid();
            dlResumeFrom = 0;
            dlReceived = 0;
            dlBuf.clear();
            postText({{"type", "task"}, {"taskId", dlTaskId},
                      {"action", "file_download"},
                      {"payload", {{"path", srcPath}, {"resumeFrom", 0},
                                   {"chunkSize", CHUNK}, {"windowSize", WINDOW}}}});
            return;
        }

        if (type == "file_ack") {

            const size_t acked = j.value("ackedOffset", size_t{0});
            if (acked > upAcked) upAcked = acked;
            if (stage == UP || stage == UP_RESUME) pumpUpload();
            return;
        }

        if (type != "task_result") return;
        const std::string data = j.value("data", "");

        if (data.find("\"sha256\"") != std::string::npos &&
            (stage == DL || stage == DL_RESUME)) {
            try {
                auto summary = json::parse(data);
                dlSha = summary.value("sha256", "");
                dlSize = summary.value("size", size_t{0});
            } catch (const std::exception&) {}
            if (stage == DL) {
                dlDone = dlSize == N && dlBuf.size() == N &&
                         ieq(dlSha, contentSha) && dlBuf == content;

                stage = UP;
                upTargetPath = dstPath;
                upResumeFrom = 0;
                upNext = upAcked = 0;
                upSeq = 0;
                upReady = upOk = false;
                upTaskId = echonode::common::generateUuid();
                postText({{"type", "task"}, {"taskId", upTaskId},
                          {"action", "file_upload"},
                          {"payload", {{"path", dstPath}, {"size", N},
                                       {"sha256", contentSha}, {"resumeFrom", 0}}}});
            } else {

                dlResumeDone = dlSize == N && ieq(dlSha, contentSha) &&
                               dlReceived == N &&
                               (dlBuf.size() >= N && dlBuf.substr(dlResumeFrom) ==
                                                        content.substr(dlResumeFrom));

                stage = UP_RESUME;
                upTargetPath = dstResume;
                upResumeFrom = N / 2;

                {
                    std::ofstream pf(dstResume, std::ios::binary | std::ios::trunc);
                    pf.write(content.data(), static_cast<std::streamsize>(N / 2));
                }
                upNext = upAcked = upResumeFrom;
                upSeq = 0;
                upReady = false;
                upResumeOk = false;
                upTaskId = echonode::common::generateUuid();
                postText({{"type", "task"}, {"taskId", upTaskId},
                          {"action", "file_upload"},
                          {"payload", {{"path", dstResume}, {"size", N},
                                       {"sha256", contentSha},
                                       {"resumeFrom", upResumeFrom}}}});
            }
            return;
        }

        if ((stage == UP || stage == UP_RESUME) && !upReady) {
            try {
                auto r = json::parse(data);
                if (r.value("ready", false)) {
                    upReady = true;
                    upIdBytes = echonode::common::uuidToBytes(j.value("taskId", ""));
                    const size_t rf = r.value("resumeFrom", size_t{0});

                    if (stage == UP_RESUME && rf != upResumeFrom) {
                        std::cout << "[FAIL] 上传续传 resumeFrom 不符: 期望 "
                                  << upResumeFrom << " 实际 " << rf << "\n";
                        ++gFailed;
                    }
                    upNext = upAcked = rf;
                    pumpUpload();
                    return;
                }
            } catch (const std::exception&) {}
        }

        if (j.value("taskId", "") == upTaskId && stage == UP && data == "ok" &&
            j.value("ok", false)) {
            upOk = true;

            stage = DL_RESUME;
            dlResumeFrom = N / 2;
            dlReceived = N / 2;
            dlBuf.assign(content.substr(0, N / 2));
            dlTaskId = echonode::common::generateUuid();
            postText({{"type", "task"}, {"taskId", dlTaskId},
                      {"action", "file_download"},
                      {"payload", {{"path", srcPath}, {"resumeFrom", dlResumeFrom},
                                   {"chunkSize", CHUNK}, {"windowSize", WINDOW}}}});
            return;
        }
        if (j.value("taskId", "") == upTaskId && stage == UP_RESUME && data == "ok" &&
            j.value("ok", false)) {
            upResumeOk = true;
            stage = DONE;
        }
    });

    try {
        server.listen(18929);
    } catch (const std::exception& e) {
        std::cout << "[FAIL] 假 Server 监听 18929 失败: " << e.what() << "\n";
        return 1;
    }
    server.start_accept();
    std::thread serverThread([&server] { server.run(); });

    echonode::core::Config cfg;
    cfg.url = "ws://127.0.0.1:18929";
    cfg.token = "test-token";
    echonode::core::Client client(cfg);

    executor::Dispatcher dispatcher;
    auto fileExecutor = std::make_unique<executor::FileExecutor>();
    auto* filePtr = fileExecutor.get();
    fileExecutor->setBinarySender(
        [&client](const void* data, size_t len) { client.sendBinary(data, len); });
    fileExecutor->setResultSender(
        [&client](const echonode::protocol::TaskResult& result) {
            client.sendText(echonode::protocol::toJson(result).dump());
        });
    fileExecutor->setTextSender(
        [&client](const std::string& text) { client.sendText(text); });
    dispatcher.add(std::move(fileExecutor));
    dispatcher.add(std::make_unique<executor::ProcessExecutor>(
        echonode::platform::createProcessOps()));

    client.setTextHook([filePtr](const nlohmann::json& j) {
        if (j.value("type", std::string{}) == "file_ack") {
            filePtr->onFileAck(j.value("taskId", std::string{}),
                               j.value("ackedOffset", size_t{0}));
            return true;
        }
        return false;
    });
    client.setBinaryHook([filePtr](const void* data, size_t len) {
        return filePtr->onBinary(data, len);
    });
    client.setTaskHandler([&dispatcher](echonode::protocol::Task task) {
        return dispatcher.dispatch(std::move(task));
    });

    std::thread clientThread([&] { client.run(); });

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return dlDone; }, 20000),
          "file_download 窗口+ack 全量回传 + sha256 校验 (2MB)");

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return upOk; }, 20000),
          "file_upload 窗口+ack 收满校验后回 ok (2MB)");

    bool dstMatches = false;
    {
        std::ifstream f(dstPath, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        dstMatches = got == content;
    }
    check(dstMatches, "file_upload 落地文件逐字节一致");

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return dlResumeDone; }, 20000),
          "file_download 断点续传（从半程只收尾部 + sha256 覆盖全文件）");

    check(waitFor([&] { std::lock_guard<std::mutex> lk(mtx); return upResumeOk; }, 20000),
          "file_upload 断点续传（预置半程文件，只传尾部，全文件 sha256 校验）");

    bool resumeMatches = false;
    {
        std::ifstream f(dstResume, std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        resumeMatches = got == content;
    }
    check(resumeMatches, "file_upload 续传落地文件逐字节一致");

    client.stop();
    clientThread.join();
    server.stop();
    serverThread.join();

    std::remove(srcPath.c_str());
    std::remove(dstPath.c_str());
    std::remove(dstResume.c_str());

    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}