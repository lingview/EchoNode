#include "EncodingWin.hpp"
#include "IProcessOps.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <memory>

namespace echonode::platform {
namespace {

class ProcessOpsWin : public IProcessOps {
public:
    std::vector<ProcessInfo> listProcesses() override {
        std::vector<ProcessInfo> out;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap == INVALID_HANDLE_VALUE) return out;

        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                ProcessInfo p;
                p.pid = pe.th32ProcessID;
                p.name = win::wideToUtf8(pe.szExeFile);
                // user 留空：取它需要 OpenProcess+GetTokenUser，大量系统进程会拒绝访问，成本高收益低
                out.push_back(std::move(p));
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
        return out;
    }

    bool killProcess(uint32_t pid) override {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (!h) return false;
        BOOL ok = TerminateProcess(h, 1);
        CloseHandle(h);
        return ok != 0;
    }
};

} // namespace

std::unique_ptr<IProcessOps> createProcessOps() {
    return std::make_unique<ProcessOpsWin>();
}

} // namespace echonode::platform
