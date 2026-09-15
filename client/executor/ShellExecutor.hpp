// shell 类动作执行器：shell_exec 一次性命令 / shell_open / shell_close
#pragma once
#include "IExecutor.hpp"
#include "ShellSessionManager.hpp"

#include "../platform/IShellSpawn.hpp"

#include <memory>

namespace echonode::executor {

class ShellExecutor : public IExecutor {
public:
    ShellExecutor(std::unique_ptr<platform::IShellSpawn> shell,
                  ShellSessionManager& sessions)
        : shell_(std::move(shell)), sessions_(sessions) {}

    std::vector<std::string> actions() const override {
        return {"shell_exec", "shell_open", "shell_close", "systeminfo"};
    }

    protocol::TaskResult execute(protocol::Task task) override;

private:
    protocol::TaskResult runShellExec(protocol::Task& task);

    std::unique_ptr<platform::IShellSpawn> shell_;
    ShellSessionManager& sessions_;
};

}
