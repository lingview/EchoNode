// 主机信息采集
#pragma once
#include "HostInfo.hpp"

namespace echonode::platform {

class ISystemInfo {
public:
    virtual ~ISystemInfo() = default;
    virtual HostInfo getHostInfo() = 0;
};

}
