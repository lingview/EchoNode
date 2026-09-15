#include "platform/PlatformFactory.hpp"
#include "platform/IShellSpawn.hpp"

#include <iostream>
#include <windows.h>

using namespace echonode::platform;

int main() {
    std::cout << std::unitbuf;
    FreeConsole();
    try {
        auto shell = createShellSpawn();
        ShellHandle h = shell->spawn("cmd.exe");
        Sleep(1500);
        std::string out;
        for (int i = 0; i < 15; ++i) { Sleep(100); shell->tryRead(h, out); }
        out.clear();

        shell->write(h, "echo marker99\r");
        for (int i = 0; i < 100; ++i) {
            Sleep(100);
            if (shell->tryRead(h, out) && !out.empty()) {
                bool hit = out.find("marker99") != std::string::npos;
                if (hit) { std::cout << "ECHO PATH WORKS\n"; break; }
            }
            if (i % 50 == 49) std::cout << "..." << i / 100 + 1 << "s alive=" << shell->alive(h) << "\n";
        }
        shell->terminate(h);
        std::cout << "done alive=" << shell->alive(h) << "\n";
    } catch (const std::exception& e) {
        std::cout << "EXCEPTION: " << e.what() << "\n";
    }
}
