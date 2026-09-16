#include "PlatformFactory.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

using namespace echonode::platform;

namespace {

int gFailed = 0;

void check(bool ok, const std::string& name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
    if (!ok) ++gFailed;
}

bool waitFor(std::function<bool()> cond, int timeoutMs) {
    for (int elapsed = 0; elapsed < timeoutMs; elapsed += 50) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

void testSystemInfo() {
    auto info = createSystemInfo()->getHostInfo();
    std::cout << "  hostname=" << info.hostname << " os=" << info.osName
              << " " << info.osVersion << " user=" << info.userName
              << " arch=" << info.arch << "\n";
    check(!info.hostname.empty() && !info.osName.empty() && !info.arch.empty(),
          "ISystemInfo::getHostInfo");
}

void testProcessOps() {
    auto procs = createProcessOps()->listProcesses();
    std::cout << "  进程数: " << procs.size() << "\n";
    for (size_t i = 0; i < procs.size() && i < 5; ++i) {
        std::cout << "    pid=" << procs[i].pid << " name=" << procs[i].name
                  << " user=" << procs[i].user << "\n";
    }
    check(!procs.empty(), "IProcessOps::listProcesses");

    check(!createProcessOps()->killProcess(0x7FFFFFFF), "IProcessOps::killProcess(不存在)");
}

void testShellSpawn(const std::string& osName) {
    auto shell = createShellSpawn();

    auto out = shell->runCommand("echo hello");
    check(out.find("hello") != std::string::npos, "IShellSpawn::runCommand");

    const char* shellPath = osName == "Windows" ? "cmd.exe" : "/bin/bash";
    const char* newline = osName == "Windows" ? "\r\n" : "\n";
    auto h = shell->spawn(shellPath);
    bool written = shell->write(h, std::string("echo marker123") + newline);
    check(written, "IShellSpawn::write");

    std::string all;
    bool gotEcho = waitFor([&] {
        std::string chunk;
        while (shell->tryRead(h, chunk)) {
            all += chunk;
            chunk.clear();
        }
        return all.find("marker123") != std::string::npos;
    }, 5000);
    check(gotEcho, "IShellSpawn::spawn + tryRead");

    check(shell->alive(h), "IShellSpawn::alive(存活)");
    shell->terminate(h);
    check(!shell->alive(h), "IShellSpawn::terminate(已终止)");
}

void testScreenCapture() {
    try {
        auto bmp = createScreenCapture()->captureBmp();
        bool valid = bmp.size() > 54 && bmp[0] == 'B' && bmp[1] == 'M';
        std::cout << "  BMP 大小: " << bmp.size() << " 字节\n";
        check(valid, "IScreenCapture::captureBmp");
    } catch (const PlatformError& e) {

        std::cout << "[SKIP] IScreenCapture::captureBmp: " << e.what() << "\n";
    }
}

void testCaptureFirstFrame() {
    for (int round = 0; round < 3; ++round) {
        try {
            auto cap = createScreenCapture();
            std::vector<uint8_t> rgb;
            int w = 0, h = 0;
            const bool fresh = cap->captureScaledRgb(rgb, w, h, 0.5);
            std::cout << "  round=" << round << " ret=" << fresh << " " << w
                      << "x" << h << " bytes=" << rgb.size() << "\n";
            check(fresh && w > 0 && h > 0 &&
                      rgb.size() == static_cast<size_t>(w) * h * 3,
                  "冷启动首帧必须给得出画面");
        } catch (const PlatformError& e) {
            std::cout << "[SKIP] 冷启动首帧: " << e.what() << "\n";
            return;
        }
    }
}

void testFrameMetadata() {
    try {
        auto cap = createScreenCapture();
        std::vector<uint8_t> rgb;
        int w = 0, h = 0;
        cap->captureScaledRgb(rgb, w, h, 0.5);
        std::vector<DirtyRect> dirty;
        std::vector<MoveRect> moves;
        const bool has = cap->lastFrameMetadata(dirty, moves);
        std::cout << "  lastFrameMetadata=" << has << " dirty=" << dirty.size()
                  << " moves=" << moves.size() << "\n";
        bool sane = true;
        for (const auto& d : dirty)
            if (d.x < 0 || d.y < 0 || d.w < 0 || d.h < 0 || d.x > w || d.y > h) sane = false;
        for (const auto& m : moves)
            if (m.sx < 0 || m.sy < 0 || m.dx < 0 || m.dy < 0 || m.w < 0 || m.h < 0 ||
                m.sx > w || m.sy > h || m.dx > w || m.dy > h)
                sane = false;
        check(sane, "IScreenCapture::lastFrameMetadata 矩形合法（或无元数据）");
    } catch (const PlatformError& e) {
        std::cout << "[SKIP] lastFrameMetadata: " << e.what() << "\n";
    }
}

}

int main() {
    auto osName = createSystemInfo()->getHostInfo().osName;
    std::cout << "== platform smoke test (" << osName << ") ==\n";
    testSystemInfo();
    testProcessOps();
    testShellSpawn(osName);
    testScreenCapture();
    testCaptureFirstFrame();
    testFrameMetadata();
    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}