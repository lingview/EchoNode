#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <string>
#include <thread>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

using HPCON_t = PVOID;
using FnCreatePseudoConsole = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON_t*);
using FnResizePseudoConsole = HRESULT(WINAPI*)(HPCON_t, COORD);
using FnClosePseudoConsole  = void (WINAPI*)(HPCON_t);

struct ConPtyApi {
    FnCreatePseudoConsole Create = nullptr;
    FnResizePseudoConsole Resize = nullptr;
    FnClosePseudoConsole  Close  = nullptr;

    bool load() {
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        Create = (FnCreatePseudoConsole)GetProcAddress(k32, "CreatePseudoConsole");
        Resize = (FnResizePseudoConsole)GetProcAddress(k32, "ResizePseudoConsole");
        Close  = (FnClosePseudoConsole )GetProcAddress(k32, "ClosePseudoConsole");
        return Create && Resize && Close;
    }
} g_conpty;

class VtNegotiationParser {
public:
    std::function<void(const std::string& params, char finalByte)> onCsi;

    void feed(const char* data, size_t len) {
        for (size_t i = 0; i < len; i++) {
            char ch = data[i];
            switch (state_) {
            case State::Ground:
                if (ch == '\x1b') state_ = State::Escape;
                break;
            case State::Escape:
                if (ch == '[') { state_ = State::Csi; params_.clear(); }
                else           { state_ = State::Ground; }
                break;
            case State::Csi:
                if ((ch >= '0' && ch <= '9') || ch == ';' || ch == '?' ||
                    ch == '<' || ch == '>' || ch == ':') {
                    if (params_.size() < 128) params_ += ch;
                }
                else if (ch >= 0x40 && ch <= 0x7E) {
                    if (onCsi) onCsi(params_, ch);
                    state_ = State::Ground;
                }
                else {
                    state_ = State::Ground;
                }
                break;
            }
        }
    }

private:
    enum class State { Ground, Escape, Csi };
    State state_ = State::Ground;
    std::string params_;
};

class Win32InputEncoder {
public:
    static std::string encodeKey(wchar_t ch, UINT vk, DWORD modifiers = 0) {
        UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        std::string out;
        out += encodeOne(ch, modifiers, vk, sc, true);
        out += encodeOne(ch, modifiers, vk, sc, false);
        return out;
    }

    static std::string encodeOne(wchar_t ch, DWORD modifiers, UINT vk,
                                 UINT sc, bool keyDown) {
        unsigned codepoint = (unsigned)ch;
        char buf[128];
        snprintf(buf, sizeof(buf), "\x1b[%u;%lu;%u;%u;%u;%u_",
                 codepoint, (unsigned long)modifiers, vk, sc,
                 codepoint, keyDown ? 1u : 0u);
        return std::string(buf);
    }

    static std::string encodeString(const std::wstring& text) {
        std::string out;
        for (wchar_t ch : text) {
            UINT vk;
            if (ch == L'\r' || ch == L'\n') vk = VK_RETURN;
            else if (ch == L'\t')           vk = VK_TAB;
            else if (ch == L'\b')           vk = VK_BACK;
            else {
                SHORT scan = VkKeyScanW(ch);
                vk = (scan == -1) ? (UINT)ch : (UINT)(scan & 0xFF);
            }
            DWORD mods = 0;
            if (iswupper(ch) && iswalpha(ch)) mods = SHIFT_PRESSED;
            out += encodeKey(ch, vk, mods);
        }
        return out;
    }
};

static std::atomic<bool> g_negotiated{false};
static std::atomic<bool> g_running{true};
static HANDLE g_hInWrite = INVALID_HANDLE_VALUE;

static void writeToConpty(const std::string& data) {
    DWORD written = 0;
    size_t off = 0;
    while (off < data.size()) {
        if (!WriteFile(g_hInWrite, data.data() + off,
                       (DWORD)(data.size() - off), &written, NULL) || written == 0)
            break;
        off += written;
    }
}

static bool createPipePair(HANDLE* readEnd, HANDLE* writeEnd) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    return CreatePipe(readEnd, writeEnd, &sa, 0) != FALSE;
}

static void outputThread(HANDLE hOutRead) {
    VtNegotiationParser parser;
    parser.onCsi = [](const std::string& params, char finalByte) {
        if (finalByte == 'h') {
            if (params == "?9001") {
                writeToConpty("\x1b[?9001;1$y");
                g_negotiated = true;
                fprintf(stderr, "[agent] Win32 Input Mode negotiated tick=%lu\n",
                        GetTickCount64());
            } else if (params == "?1004") {
                writeToConpty("\x1b[?1004;1$y");
                fprintf(stderr, "[agent] Focus Events negotiated\n");
            }
        }
    };

    char buf[4096];
    while (g_running) {
        DWORD avail = 0;
        if (!PeekNamedPipe(hOutRead, NULL, 0, NULL, &avail, NULL) || avail == 0) {
            Sleep(5);
            continue;
        }
        DWORD n = 0;
        if (!ReadFile(hOutRead, buf, sizeof(buf), &n, NULL) || n == 0)
            break;

        parser.feed(buf, n);
        fwrite(buf, 1, n, stdout);
        fflush(stdout);
    }
}

static void inputThread() {
    for (int i = 0; i < 300 && !g_negotiated; i++) Sleep(10);

    if (!g_negotiated) {
        fprintf(stderr, "[agent] WARNING: negotiation not seen, "
                        "falling back to plain text\n");
    }

    Sleep(200);

    std::wstring cmdLine = L"echo hi\r";
    writeToConpty(Win32InputEncoder::encodeString(cmdLine));
    Sleep(500);

    writeToConpty(Win32InputEncoder::encodeString(L"ver\r"));
    Sleep(500);

    Sleep(3000);
    writeToConpty(Win32InputEncoder::encodeString(L"exit\r"));
}

int wmain() {
    if (!g_conpty.load()) {
        fprintf(stderr, "ConPTY API not available\n");
        return 1;
    }

    HANDLE hInRead  = INVALID_HANDLE_VALUE, hInWrite  = INVALID_HANDLE_VALUE;
    HANDLE hOutRead = INVALID_HANDLE_VALUE, hOutWrite = INVALID_HANDLE_VALUE;
    if (!createPipePair(&hInRead, &hInWrite))  return 1;
    if (!createPipePair(&hOutRead, &hOutWrite)) return 1;
    g_hInWrite = hInWrite;

    // 预发协商应答：cmd 读 stdin 前完成协商，避免输入被丢弃
    writeToConpty("[?9001;1$y[?1004;1$y");

    HPCON_t hPC = nullptr;
    COORD size{80, 24};
    HRESULT hr = g_conpty.Create(size, hInRead, hOutWrite, 0, &hPC);
    if (hr != S_OK) {
        fprintf(stderr, "CreatePseudoConsole failed: 0x%08lx\n", hr);
        return 1;
    }
    CloseHandle(hInRead);
    CloseHandle(hOutWrite);

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
    auto attrList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(
        GetProcessHeap(), 0, attrSize);
    if (!attrList) return 1;
    if (!InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)) return 1;
    if (!UpdateProcThreadAttribute(attrList, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            hPC, sizeof(HPCON_t), NULL, NULL)) return 1;

    // 协商线程必须先于 CreateProcess：conhost 在 CreatePseudoConsole 时即发 ?9001h，
    // 应答窗口极短，晚了 cmd 以 EOF 退出
    std::thread tout(outputThread, hOutRead);
    for (int i = 0; i < 200 && !g_negotiated; i++) Sleep(10);
    fprintf(stderr, "[agent] negotiation settled (win32=%d)\n", (int)g_negotiated);

    STARTUPINFOEXW siEx{};
    siEx.StartupInfo.cb = sizeof(siEx);
    siEx.lpAttributeList = attrList;

    PROCESS_INFORMATION pi{};
    wchar_t cmdline[] = L"cmd.exe";
    BOOL ok = CreateProcessW(
        NULL, cmdline, NULL, NULL,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT,
        NULL, NULL,
        &siEx.StartupInfo, &pi);

    if (!ok) {
        fprintf(stderr, "CreateProcessW failed: %lu\n", GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);

    fprintf(stderr, "[agent] cmd.exe started (pid=%lu)\n", pi.dwProcessId);

    std::thread tin(inputThread);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    fprintf(stderr, "\n[agent] child exited, code=%lu tick=%lu (now=%lu)\n",
            exitCode, GetTickCount64() - 6000, GetTickCount64());

    g_running = false;
    tin.join();
    tout.join();
    g_conpty.Close(hPC);
    CloseHandle(pi.hProcess);
    CloseHandle(hInWrite);
    CloseHandle(hOutRead);
    DeleteProcThreadAttributeList(attrList);
    HeapFree(GetProcessHeap(), 0, attrList);
    return (int)exitCode;
}
