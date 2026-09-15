// 任务执行器接口：一个实现可声明处理多个 action
#pragma once
#include "protocol/from_server.hpp"
#include "protocol/to_server.hpp"

#include <string>
#include <vector>

namespace echonode::executor {

class IExecutor {
public:
    virtual ~IExecutor() = default;

    virtual std::vector<std::string> actions() const = 0;

    // 返回的 TaskResult.taskId 必须与入参一致
    virtual protocol::TaskResult execute(protocol::Task task) = 0;
};

}
