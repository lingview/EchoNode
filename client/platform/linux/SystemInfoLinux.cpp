#include "ISystemInfo.hpp"

#include <pwd.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <memory>

namespace echonode::platform {
namespace {

class SystemInfoLinux : public ISystemInfo {
public:
    HostInfo getHostInfo() override {
        HostInfo info;
        info.osName = "Linux";

        char buf[256]{};
        if (gethostname(buf, sizeof(buf) - 1) == 0) info.hostname = buf;

        utsname un{};
        if (uname(&un) == 0) {
            info.osVersion = un.release;
            info.arch = un.machine; // x86_64 / aarch64，与 protocol.md 口径一致
        }
        if (passwd* pw = getpwuid(geteuid())) info.userName = pw->pw_name;
        return info;
    }
};

} // namespace

std::unique_ptr<ISystemInfo> createSystemInfo() {
    return std::make_unique<SystemInfoLinux>();
}

} // namespace echonode::platform
