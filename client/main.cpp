#include "core/Client.hpp"
#include "core/Config.hpp"
#include "executor/Dispatcher.hpp"
#include "executor/FileExecutor.hpp"
#include "executor/ScreenshotExecutor.hpp"
#include "executor/RemoteDeskExecutor.hpp"
#include "executor/ShellExecutor.hpp"
#include "executor/ProcessExecutor.hpp"
#include "executor/ShellSessionManager.hpp"
#include "platform/PlatformFactory.hpp"
#include "protocol/to_server.hpp"

#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace executor = echonode::executor;

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>

static void initConsoleUtf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

static void setupStdio(bool showConsole) {
    if (showConsole) {
        AllocConsole();
        return;
    }
    FILE* tmp = nullptr;
    if (freopen_s(&tmp, "agent.log", "a", stdout) == 0) {
        _dup2(_fileno(stdout), _fileno(stderr));
    }
}
#else
static void initConsoleUtf8() {}
static void setupStdio(bool) {}
#endif

static bool hasFlag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == flag) return true;
    return false;
}

static int agentMain(int argc, char** argv) {
    setupStdio(hasFlag(argc, argv, "--console"));
    initConsoleUtf8();

    auto cfg = echonode::core::Config::fromArgs(argc, argv);
    if (!cfg.valid) {
        std::cerr << "配置错误: " << cfg.error << "\n" << echonode::core::Config::usage() << "\n";
        return 1;
    }

    echonode::core::Client client(cfg);

    executor::Dispatcher dispatcher;
    auto sessions =
        std::make_unique<echonode::executor::ShellSessionManager>(echonode::platform::createShellSpawn());
    auto* sessionsPtr = sessions.get();
    sessions->setSink([&client](const std::string& sessionId, const std::string& data, bool eof) {
        client.sendText(echonode::protocol::toJson(
                            echonode::protocol::ShellData{sessionId, data, eof})
                            .dump());
    });
    dispatcher.add(std::make_unique<echonode::executor::ShellExecutor>(
        echonode::platform::createShellSpawn(), *sessions));
    dispatcher.add(std::make_unique<echonode::executor::ProcessExecutor>(
        echonode::platform::createProcessOps()));

    auto fileExecutor = std::make_unique<echonode::executor::FileExecutor>();
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
    client.setBinaryHook([filePtr](const void* data, size_t len) {
        return filePtr->onBinary(data, len);
    });

    auto shotExecutor = std::make_unique<echonode::executor::ScreenshotExecutor>(
        echonode::platform::createScreenCapture());
    shotExecutor->setBinarySender(
        [&client](const void* data, size_t len) { client.sendBinary(data, len); });
    dispatcher.add(std::move(shotExecutor));

    auto deskExecutor = std::make_unique<echonode::executor::RemoteDeskExecutor>();
    deskExecutor->setBinarySender(
        [&client](const void* data, size_t len) { client.sendBinary(data, len); });
    deskExecutor->setResultSender(
        [&client](const echonode::protocol::TaskResult& result) {
            client.sendText(echonode::protocol::toJson(result).dump());
        });
    deskExecutor->setInputInjector(echonode::platform::createInputInjector());
    dispatcher.add(std::move(deskExecutor));

    client.setTextHook([&sessionsPtr, filePtr](const nlohmann::json& j) {
        const std::string type = j.value("type", std::string{});
        if (type == "file_ack") {
            filePtr->onFileAck(j.value("taskId", std::string{}),
                               j.value("ackedOffset", size_t{0}));
            return true;
        }
        if (type == "shell_resize") {
            sessionsPtr->resize(j.value("sessionId", std::string{}),
                                j.value("cols", 80), j.value("rows", 24));
            return true;
        }
        if (type != "shell_data") return false;
        if (j.value("eof", false)) {
            sessionsPtr->close(j.value("sessionId", std::string{}));
            return true;
        }
        sessionsPtr->input(j.value("sessionId", std::string{}),
                           j.value("data", std::string{}));
        return true;
    });
    client.setTaskHandler([&dispatcher](echonode::protocol::Task task) {
        return dispatcher.dispatch(std::move(task));
    });

    client.run();
    return 0;
}

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    args.reserve(argc > 0 ? argc : 1);
    for (int i = 0; i < argc; ++i) {
        const int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(need > 1 ? need - 1 : 0, '\0');
        if (need > 1)
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, s.data(), need, nullptr, nullptr);
        args.push_back(std::move(s));
    }
    if (wargv) LocalFree(wargv);
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& a : args) argv.push_back(a.data());
    return agentMain(static_cast<int>(argv.size()), argv.data());
}
#else
int main(int argc, char** argv) { return agentMain(argc, argv); }
#endif
