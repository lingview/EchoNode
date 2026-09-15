#include "EncodingWin.hpp"
#include "ISystemInfo.hpp"

#include <windows.h>
#include <memory>

namespace echonode::platform {
namespace {

std::string realOsVersion() {
    // GetVersionEx 受 manifest 限制会谎报版本，走 ntdll 的 RtlGetVersion 拿真实值
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return {};
    using Fn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!fn) return {};
    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (fn(&info) != 0) return {};
    return std::to_string(info.dwMajorVersion) + "." +
           std::to_string(info.dwMinorVersion) + "." +
           std::to_string(info.dwBuildNumber);
}

class SystemInfoWin : public ISystemInfo {
public:
    HostInfo getHostInfo() override {
        HostInfo info;
        info.osName = "Windows";
        info.osVersion = realOsVersion();

        wchar_t host[MAX_COMPUTERNAME_LENGTH + 1]{};
        DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
        if (GetComputerNameW(host, &len)) info.hostname = win::wideToUtf8(host);

        wchar_t user[256]{};
        DWORD ulen = 256;
        if (GetUserNameW(user, &ulen)) info.userName = win::wideToUtf8(user);

        SYSTEM_INFO si{};
        GetNativeSystemInfo(&si);
        switch (si.wProcessorArchitecture) {
            case PROCESSOR_ARCHITECTURE_AMD64: info.arch = "x86_64"; break;
            case PROCESSOR_ARCHITECTURE_ARM64: info.arch = "arm64"; break;
            case PROCESSOR_ARCHITECTURE_INTEL: info.arch = "x86"; break;
            default: info.arch = "unknown"; break;
        }
        return info;
    }
};

} // namespace

std::unique_ptr<ISystemInfo> createSystemInfo() {
    return std::make_unique<SystemInfoWin>();
}

} // namespace echonode::platform
