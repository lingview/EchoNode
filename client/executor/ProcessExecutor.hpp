// 进程类动作执行器：ps_list 进程列表 / ps_kill 结束进程
#pragma once
#include "IExecutor.hpp"

#include "../platform/IProcessOps.hpp"

#include <memory>

namespace echonode::executor {

class ProcessExecutor : public IExecutor {
public:
    explicit ProcessExecutor(std::unique_ptr<platform::IProcessOps> ops)
        : ops_(std::move(ops)) {}

    std::vector<std::string> actions() const override {
        return {"ps_list", "ps_kill"};
    }

    protocol::TaskResult execute(protocol::Task task) override;

private:
    std::unique_ptr<platform::IProcessOps> ops_;
};

}
