// 进程条目。user 取不到时留空（不视为错误）
#pragma once
#include <cstdint>
#include <string>

namespace echonode::platform {

struct ProcessInfo {
    uint32_t pid = 0;
    std::string name;
    std::string user;
};

}
