#include "EncodingWin.hpp"
#include "IShellSpawn.hpp"

#include <windows.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace echonode::platform {
namespace {

using HPC = void*;
using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPC*);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(HPC, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(HPC);

struct ConPtyApi {
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;

    static const ConPtyApi& get() {
        static ConPtyApi api = [] {
            HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
            return ConPtyApi{
                reinterpret_cast<CreatePseudoConsoleFn>(
                    GetProcAddress(k32, "CreatePseudoConsole")),
                reinterpret_cast<ResizePseudoConsoleFn>(
                    GetProcAddress(k32, "ResizePseudoConsole")),
                reinterpret_cast<ClosePseudoConsoleFn>(
                    GetProcAddress(k32, "ClosePseudoConsole")),
            };
        }();
        return api;
    }
};

constexpr DWORD kPseudoConsoleAttribute = 0x00020016;

struct WinSession {
    PROCESS_INFORMATION pi{};
    HPC hpc = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    SIZE_T attrSize = 0;
    void* attrList = nullptr;
    bool win32Input = false;
    std::string vtTail;
};

UINT vkForChar(wchar_t ch) {
    switch (ch) {
    case L'\r': return VK_RETURN;
    case L'\n': return VK_RETURN;
    case L'\t': return VK_TAB;
    case L'\b': case 0x7F: return VK_BACK;
    case 0x1B: return VK_ESCAPE;
    default: break;
    }
    SHORT scan = VkKeyScanW(ch);
    return scan == -1 ? static_cast<UINT>(ch) : static_cast<UINT>(scan & 0xFF);
}

std::string encodeKeyEvent(unsigned codepoint, UINT vk, DWORD mods, bool down) {
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    char buf[96];
    snprintf(buf, sizeof(buf), "\x1b[%u;%u;%u;%u;%lu;%u_",
             vk, sc, codepoint, down ? 1u : 0u,
             static_cast<unsigned long>(mods), 1u);
    return std::string(buf);
}

std::string encodeSpecialControls(const std::string& data) {
    std::string out;
    out.reserve(data.size() * 4);
    for (unsigned char uc : data) {
        if (uc >= 1 && uc <= 26 && uc != 9 && uc != 10 && uc != 13 && uc != 27) {
            const UINT vk = static_cast<UINT>('A' + uc - 1);
            out += encodeKeyEvent(uc, vk, LEFT_CTRL_PRESSED, true);
            out += encodeKeyEvent(uc, vk, LEFT_CTRL_PRESSED, false);
        } else {
            out += static_cast<char>(uc);
        }
    }
    return out;
}

void scanWin32InputMode(WinSession* s, const std::string& data) {
    if (s->win32Input) return;
    std::string joined = s->vtTail + data;
    if (joined.find("\x1b[?9001h") != std::string::npos) s->win32Input = true;
    s->vtTail = joined.substr(joined.size() > 24 ? joined.size() - 24 : 0);
}

bool isValidUtf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t need = 0;
        if (c < 0x80) need = 0;
        else if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return false;
        if (i + need >= s.size() + 1 && need > 0) {
            if (i + need > s.size()) return false;
        }
        for (size_t k = 1; k <= need; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += need + 1;
    }
    return true;
}

class ShellSpawnWin : public IShellSpawn {
public:
    std::string runCommand(const std::string& command) override {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 0)) throw PlatformError("CreatePipe failed");
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        HANDLE nulIn = CreateFileW(L"NUL", GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);

        std::wstring line = L"cmd.exe /C ";
        line += win::utf8ToWide(command);
        line += L" 2>&1";
        std::vector<wchar_t> cmdBuf(line.begin(), line.end());
        cmdBuf.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput = nulIn;
        si.hStdOutput = wr;
        si.hStdError = wr;

        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(wr);
        if (!ok) {
            if (nulIn != INVALID_HANDLE_VALUE) CloseHandle(nulIn);
            CloseHandle(rd);
            throw PlatformError("CreateProcess failed: " + std::to_string(GetLastError()));
        }

        std::string raw;
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) raw.append(buf, n);
        WaitForSingleObject(pi.hProcess, INFINITE);
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) raw.append(buf, n);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(rd);
        if (nulIn != INVALID_HANDLE_VALUE) CloseHandle(nulIn);

        if (isValidUtf8(raw)) return raw;
        return win::codepageToUtf8(raw, CP_OEMCP);
    }

    ShellHandle spawn(const std::string& shellPath) override {
        const auto& api = ConPtyApi::get();
        if (!api.create || !api.resize || !api.close)
            throw PlatformError("ConPTY not supported (need Win10 1809+)");

        HANDLE inRd = nullptr, inWr = nullptr, outRd = nullptr, outWr = nullptr;
        if (!CreatePipe(&inRd, &inWr, nullptr, 0) || !CreatePipe(&outRd, &outWr, nullptr, 0)) {
            throw PlatformError("CreatePipe failed: " + std::to_string(GetLastError()));
        }
        SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);

        auto sess = std::make_unique<WinSession>();
        COORD size{80, 24};
        HRESULT hr = api.create(size, inRd, outWr, 0, &sess->hpc);
        CloseHandle(inRd);
        CloseHandle(outWr);
        if (FAILED(hr)) {
            CloseHandle(inWr);
            CloseHandle(outRd);
            throw PlatformError("CreatePseudoConsole failed: " + std::to_string(hr));
        }

        sess->attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &sess->attrSize);
        sess->attrList = HeapAlloc(GetProcessHeap(), 0, sess->attrSize);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(sess->attrList);
        const BOOL init2 = InitializeProcThreadAttributeList(list, 1, 0, &sess->attrSize);
        const BOOL upd = UpdateProcThreadAttribute(list, 0, kPseudoConsoleAttribute,
                                  sess->hpc, sizeof(HPC), nullptr, nullptr);
        if (!init2 || !upd)
            std::cerr << "[pty] 伪终端属性失败 init2=" << init2
                      << " update=" << upd << " err=" << GetLastError() << std::endl;

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nullptr;
        si.StartupInfo.hStdOutput = nullptr;
        si.StartupInfo.hStdError = nullptr;
        si.lpAttributeList = list;

        std::wstring cmd = win::utf8ToWide(shellPath);
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                                 EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                 &si.StartupInfo, &sess->pi);
        DeleteProcThreadAttributeList(list);
        HeapFree(GetProcessHeap(), 0, sess->attrList);
        sess->attrList = nullptr;
        if (!ok) {
            api.close(sess->hpc);
            CloseHandle(inWr);
            CloseHandle(outRd);
            throw PlatformError("CreateProcess failed: " + std::to_string(GetLastError()));
        }
        sess->stdinWrite = inWr;
        sess->stdoutRead = outRd;
        return sess.release();
    }

    bool resize(ShellHandle handle, int cols, int rows) override {
        const auto& api = ConPtyApi::get();
        if (!api.resize) return false;
        COORD size{static_cast<short>(cols), static_cast<short>(rows)};
        return SUCCEEDED(api.resize(static_cast<WinSession*>(handle)->hpc, size));
    }

    bool interrupt(ShellHandle handle) override {
        auto* s = static_cast<WinSession*>(handle);
        return GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, s->pi.dwProcessId) != 0;
    }

    bool write(ShellHandle handle, std::string_view data) override {
        auto* s = static_cast<WinSession*>(handle);
        if (data.find('\x03') != std::string_view::npos) {
            interrupt(handle);
            std::string stripped;
            for (char c : data) if (c != '\x03') stripped += c;
            if (stripped.empty()) return true;
            data = stripped;
        }
        std::string payload = encodeSpecialControls(std::string(data));
        DWORD written = 0;
        if (!WriteFile(s->stdinWrite, payload.data(), static_cast<DWORD>(payload.size()),
                       &written, nullptr)) {
            return false;
        }
        return written == payload.size();
    }

    bool tryRead(ShellHandle handle, std::string& out) override {
        auto* s = static_cast<WinSession*>(handle);
        DWORD avail = 0;
        if (!PeekNamedPipe(s->stdoutRead, nullptr, 0, nullptr, &avail, nullptr))
            return false;
        if (avail == 0) return false;

        char buf[4096];
        DWORD toRead = avail < sizeof(buf) ? avail : sizeof(buf);
        DWORD n = 0;
        if (!ReadFile(s->stdoutRead, buf, toRead, &n, nullptr) || n == 0)
            return false;
        out.append(buf, n);
        scanWin32InputMode(s, out);
        return true;
    }

    bool alive(ShellHandle handle) override {
        auto* s = static_cast<WinSession*>(handle);
        return WaitForSingleObject(s->pi.hProcess, 0) == WAIT_TIMEOUT;
    }

    void terminate(ShellHandle handle) override {
        auto* s = static_cast<WinSession*>(handle);
        const auto& api = ConPtyApi::get();
        if (s->hpc && api.close) api.close(s->hpc);
        DWORD code = 0;
        GetExitCodeProcess(s->pi.hProcess, &code);
        TerminateProcess(s->pi.hProcess, 0);
        WaitForSingleObject(s->pi.hProcess, 2000);
        CloseHandle(s->pi.hThread);
        CloseHandle(s->pi.hProcess);
        CloseHandle(s->stdinWrite);
        CloseHandle(s->stdoutRead);
        delete s;
    }
};

} // namespace

std::unique_ptr<IShellSpawn> createShellSpawn() {
    return std::make_unique<ShellSpawnWin>();
}

} // namespace echonode::platform
