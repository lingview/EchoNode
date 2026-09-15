#include "Error.hpp"
#include "IProcessOps.hpp"

#include <dirent.h>
#include <pwd.h>
#include <signal.h>
#include <sys/types.h>
#include <memory>
#include <fstream>
#include <sstream>

namespace echonode::platform {
namespace {

std::string readComm(long pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    std::getline(f, name);
    return name;
}

std::string readUser(long pid) {
    std::ifstream f("/proc/" + std::to_string(pid) + "/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("Uid:", 0) != 0) continue;
        // "Uid:\t<real>\t<effective>\t..."，取第一个 real uid
        uid_t uid = 0;
        std::istringstream ss(line.substr(4));
        ss >> uid;
        if (passwd* pw = getpwuid(uid)) return pw->pw_name;
        return std::to_string(uid); // 查不到用户名时退回 uid
    }
    return {};
}

class ProcessOpsLinux : public IProcessOps {
public:
    std::vector<ProcessInfo> listProcesses() override {
        std::vector<ProcessInfo> out;
        DIR* dir = opendir("/proc");
        if (!dir) throw PlatformError("opendir /proc failed");

        while (dirent* e = readdir(dir)) {
            if (e->d_type != DT_DIR) continue;
            char* end = nullptr;
            long pid = strtol(e->d_name, &end, 10);
            if (!end || *end != '\0' || pid <= 0) continue; // 只要数字目录

            ProcessInfo p;
            p.pid = static_cast<uint32_t>(pid);
            p.name = readComm(pid);
            p.user = readUser(pid);
            out.push_back(std::move(p));
        }
        closedir(dir);
        return out;
    }

    bool killProcess(uint32_t pid) override {
        return kill(static_cast<pid_t>(pid), SIGKILL) == 0;
    }
};

} // namespace

std::unique_ptr<IProcessOps> createProcessOps() {
    return std::make_unique<ProcessOpsLinux>();
}

} // namespace echonode::platform
