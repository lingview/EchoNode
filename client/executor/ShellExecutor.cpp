#include "ShellExecutor.hpp"

#include <iostream>

namespace echonode::executor {

protocol::TaskResult ShellExecutor::execute(protocol::Task task) {
    try {
        if (task.action == "shell_exec") return runShellExec(task);
        if (task.action == "shell_open") {
            const std::string shell = task.payload.value("shell", std::string{});
            // PlatformError（如 spawn 失败）转成 ok=false 结果
            return {task.taskId, true, sessions_.open(shell), {}};
        }
        if (task.action == "shell_close") {
            sessions_.close(task.payload.value("sessionId", std::string{}));
            return {task.taskId, true, "ok", {}};
        }
        if (task.action == "systeminfo") {
            // 一次性系统信息采集（Windows=systeminfo / Linux=组合命令），走 runCommand
            return {task.taskId, true,
                    shell_->runCommand(task.payload.value("command", std::string{})), {}};
        }
        return {task.taskId, false, {}, "unknown shell action"};
    } catch (const platform::PlatformError& e) {
        return {task.taskId, false, {}, e.what()};
    }
}

protocol::TaskResult ShellExecutor::runShellExec(protocol::Task& task) {
    const auto command = task.payload.value("command", std::string{});
    if (command.empty()) {
        return {task.taskId, false, {}, "payload.command is required"};
    }
    return {task.taskId, true, shell_->runCommand(command), {}};
}

} // namespace echonode::executor
