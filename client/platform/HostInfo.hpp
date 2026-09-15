// 主机信息，register 消息的数据来源，字段口径见 docs/architecture/protocol.md
#pragma once
#include <string>

namespace echonode::platform {

struct HostInfo {
    std::string hostname;
    std::string osName;    // "Windows" / "Linux"
    std::string osVersion; // 如 "10.0.26100" / "6.8.0-90-generic"
    std::string userName;
    std::string arch;      // "x86_64" / "arm64"
};

}
