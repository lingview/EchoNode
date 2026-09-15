// 进程查询与终止
#pragma once
#include <vector>
#include "ProcessInfo.hpp"

namespace echonode::platform {

class IProcessOps {
public:
    virtual ~IProcessOps() = default;

    virtual std::vector<ProcessInfo> listProcesses() = 0;

    // 目标不存在或无权限时返回 false，不抛异常（杀进程失败是常态而非错误）
    virtual bool killProcess(uint32_t pid) = 0;
};

}
