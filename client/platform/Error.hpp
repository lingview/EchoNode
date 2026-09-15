// platform 层统一错误出口：所有系统调用失败都转成它，不让 GetLastError/errno 穿透上层
#pragma once
#include <stdexcept>

namespace echonode::platform {

class PlatformError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

}
