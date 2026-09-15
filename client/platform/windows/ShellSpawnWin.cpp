#include "EncodingWin.hpp"
#include "IShellSpawn.hpp"

#include <windows.h>

#include <cstring>
#include <iostream>  // 仅伪终端属性失败时输出一行
#include <memory>

namespace echonode::platform {
namespace {

// ConPTY（Win10 1809+）伪终端：全屏程序的 TTY 检测/行编辑/回显由 conhost 提供。
// MinGW SDK 无伪终端头文件，动态加载。
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
    HANDLE stdinWrite = nullptr;  // 写 conpty 输入缓冲
    HANDLE stdoutRead = nullptr;  // 读 conpty 渲染输出
    SIZE_T attrSize = 0;
    void* attrList = nullptr;
    bool win32Input = false;   // conhost 发出 ?9001h 后为 true：输入需用 KEY_EVENT 编码
    std::string vtTail;        // 跨读取块保留尾部字节，用于检测可能被分片的协商序列
};

// ---- Win32 Input Mode 输入编码 ----
// conhost 发出 ESC[?9001h 后，输入必须为 KEY_EVENT 编码，纯文本会被丢弃

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
    // 参数顺序：Vk;Sc;Uc;Kd;Mods;Repeat，错了 conhost 会字段错位
    char buf[96];
    snprintf(buf, sizeof(buf), "\x1b[%u;%u;%u;%u;%lu;%u_",
             vk, sc, codepoint, down ? 1u : 0u,
             static_cast<unsigned long>(mods), 1u);
    return std::string(buf);
}

// 仅对 conhost 会丢弃的裸控制字节（Ctrl+字母）做 KEY_EVENT 编码，其余原样透传
std::string encodeSpecialControls(const std::string& data) {
    std::string out;
    out.reserve(data.size() * 4);
    for (unsigned char uc : data) {
        // 1-26 = Ctrl+字母（排除 Tab/LF 等由 conhost 原生处理的键）
        if (uc >= 1 && uc <= 26 && uc != 9 && uc != 10 && uc != 13 && uc != 27) {
            const UINT vk = static_cast<UINT>('A' + uc - 1);
            // 真键盘 Ctrl+C 的 KEY_EVENT：UnicodeChar = 控制字符本身
            out += encodeKeyEvent(uc, vk, LEFT_CTRL_PRESSED, true);
            out += encodeKeyEvent(uc, vk, LEFT_CTRL_PRESSED, false);
        } else {
            out += static_cast<char>(uc);
        }
    }
    return out;
}

// 扫描 conhost 输出中的 ESC[?9001h（win32-input-mode 启用声明）
void scanWin32InputMode(WinSession* s, const std::string& data) {
    if (s->win32Input) return;
    std::string joined = s->vtTail + data;
    if (joined.find("\x1b[?9001h") != std::string::npos) s->win32Input = true;
    // 保留尾部，防止序列被读取块边界分片
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
        // _popen 输出是 OEM 代码页（中文系统 936），统一转 UTF-8
        FILE* pipe = _popen((command + " 2>&1").c_str(), "r");
        if (!pipe) throw PlatformError("_popen failed: " + command);
        std::string raw;
        char buf[4096];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) raw.append(buf, n);
        _pclose(pipe);
        // 输出已是合法 UTF-8 时直通，避免二次转码乱码
        if (isValidUtf8(raw)) return raw;
        return win::codepageToUtf8(raw, CP_OEMCP);
    }

    ShellHandle spawn(const std::string& shellPath) override {
        const auto& api = ConPtyApi::get();
        if (!api.create || !api.resize || !api.close)
            throw PlatformError("ConPTY not supported (need Win10 1809+)");

        // ConPTY 经 conhost 中转，无需句柄继承
        HANDLE inRd = nullptr, inWr = nullptr, outRd = nullptr, outWr = nullptr;
        if (!CreatePipe(&inRd, &inWr, nullptr, 0) || !CreatePipe(&outRd, &outWr, nullptr, 0)) {
            throw PlatformError("CreatePipe failed: " + std::to_string(GetLastError()));
        }
        // 父进程持有的两端不可继承
        SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);

        auto sess = std::make_unique<WinSession>();
        COORD size{80, 24}; // 初始尺寸，后续经 shell_resize 下发
        HRESULT hr = api.create(size, inRd, outWr, 0, &sess->hpc);
        CloseHandle(inRd);
        CloseHandle(outWr);
        if (FAILED(hr)) {
            CloseHandle(inWr);
            CloseHandle(outRd);
            throw PlatformError("CreatePseudoConsole failed: " + std::to_string(hr));
        }

        // 子进程必须挂 PSEUDOCONSOLE 属性才会跑在伪终端里
        sess->attrSize = 0;
        // 首次调用按约定必然返回 0，只为探明属性表缓冲区大小
        InitializeProcThreadAttributeList(nullptr, 1, 0, &sess->attrSize);
        sess->attrList = HeapAlloc(GetProcessHeap(), 0, sess->attrSize);
        auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(sess->attrList);
        const BOOL init2 = InitializeProcThreadAttributeList(list, 1, 0, &sess->attrSize);
        const BOOL upd = UpdateProcThreadAttribute(list, 0, kPseudoConsoleAttribute,
                                  sess->hpc, sizeof(HPC), nullptr, nullptr);
        // 属性表/伪终端属性没挂上子进程会跑在真控制台里（管道读写全断），留痕迹
        if (!init2 || !upd)
            std::cerr << "[pty] 伪终端属性失败 init2=" << init2
                      << " update=" << upd << " err=" << GetLastError() << std::endl;

        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        // 关键：显式置空 stdio 并声明 USESTDHANDLES，否则子进程继承父进程 stdin 会秒退
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nullptr;
        si.StartupInfo.hStdOutput = nullptr;
        si.StartupInfo.hStdError = nullptr;
        si.lpAttributeList = list;

        // 命令行形式而非 lpApplicationName：后者不做 PATH 搜索
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

    // 调整伪终端尺寸（Ctrl+C 中断另见 interrupt，走 CTRL_BREAK_EVENT）
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
        // Ctrl+C 改发控制台事件（KEY_EVENT 编码在 conhost 输入桥不可靠），其余透传
        if (data.find('\x03') != std::string_view::npos) {
            interrupt(handle);
            std::string stripped;
            for (char c : data) if (c != '\x03') stripped += c;
            if (stripped.empty()) return true;
            data = stripped;
        }
        // conhost 启用 win32-input-mode 后，普通字符照常透传
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
            return false; // 管道断开视作暂无数据，alive 会判定会话结束
        if (avail == 0) return false;

        char buf[4096];
        DWORD toRead = avail < sizeof(buf) ? avail : sizeof(buf);
        DWORD n = 0;
        if (!ReadFile(s->stdoutRead, buf, toRead, &n, nullptr) || n == 0)
            return false;
        out.append(buf, n);
        scanWin32InputMode(s, out); // 检测 conhost 的 win32-input-mode 启用声明
        return true;
    }

    bool alive(ShellHandle handle) override {
        auto* s = static_cast<WinSession*>(handle);
        return WaitForSingleObject(s->pi.hProcess, 0) == WAIT_TIMEOUT;
    }

    void terminate(ShellHandle handle) override {
        auto* s = static_cast<WinSession*>(handle);
        const auto& api = ConPtyApi::get();
        if (s->hpc && api.close) api.close(s->hpc); // 关 conpty 使子进程收到关闭
        DWORD code = 0;
        GetExitCodeProcess(s->pi.hProcess, &code); // 下面统一兼底强杀，退出码无需上报
        TerminateProcess(s->pi.hProcess, 0);        // 兜底强杀
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
