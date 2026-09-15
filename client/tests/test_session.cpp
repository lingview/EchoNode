#include "executor/ShellSessionManager.hpp"
#include "platform/PlatformFactory.hpp"

#include <chrono>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace executor = echonode::executor;

int main() {
    std::cout << std::unitbuf;

    auto mgr = std::make_unique<executor::ShellSessionManager>(
        echonode::platform::createShellSpawn());

    std::mutex mtx;
    std::string all;
    bool gotEof = false;
    mgr->setSink([&](const std::string& id, const std::string& data, bool eof) {
        std::lock_guard<std::mutex> lk(mtx);
        if (!eof) all += data;
        gotEof = gotEof || eof;
    });

    const auto id = mgr->open("");

    auto info = echonode::platform::createSystemInfo()->getHostInfo();
    const std::string nl = info.osName == "Windows" ? "\r\n" : "\n";
    mgr->input(id, "echo hello_mgr" + nl);

    bool got = false;
    for (int elapsed = 0; elapsed < 5000 && !got; elapsed += 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::lock_guard<std::mutex> lk(mtx);
        got = all.find("hello_mgr") != std::string::npos;
    }
    std::cout << (got ? "[PASS] " : "[FAIL] ") << "会话输出读到 hello_mgr\n";

    mgr->close(id);
    std::cout << (gotEof ? "[PASS] " : "[FAIL] ") << "close 后收到 eof\n";

    mgr.reset();
    return got ? 0 : 1;
}
