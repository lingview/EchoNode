// SendInput 实现：鼠标（归一化→像素）+ 键盘（浏览器 KeyboardEvent.code → VK）
#ifdef _WIN32
#include "IInputInjector.hpp"
#include <windows.h>
#include <memory>

namespace echonode::platform {
namespace {

// 屏幕像素尺寸（主屏）
void screenMetrics(int& w, int& h) {
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
}

// 浏览器 code → Windows VK。覆盖常用键：字母/数字/方向/控制键
WORD codeToVk(const std::string& code) {
    if (code.size() == 4 && code.rfind("Key", 0) == 0) {
        const char c = code[3];
        if (c >= 'A' && c <= 'Z') return VkKeyScanA(c) & 0xFF; // 主布局字母
    }
    if (code.size() == 6 && code.rfind("Digit", 0) == 0) {
        const char c = code[5];
        if (c >= '0' && c <= '9') return '0' + (c - '0');
    }
    if (code == "Backspace") return VK_BACK;
    if (code == "Tab") return VK_TAB;
    if (code == "Enter") return VK_RETURN;
    if (code == "ShiftLeft") return VK_SHIFT;
    if (code == "ShiftRight") return VK_RSHIFT;
    if (code == "ControlLeft") return VK_CONTROL;
    if (code == "ControlRight") return VK_RCONTROL;
    if (code == "AltLeft") return VK_MENU;
    if (code == "Escape") return VK_ESCAPE;
    if (code == "Space") return VK_SPACE;
    if (code == "ArrowUp") return VK_UP;
    if (code == "ArrowDown") return VK_DOWN;
    if (code == "ArrowLeft") return VK_LEFT;
    if (code == "ArrowRight") return VK_RIGHT;
    if (code == "Delete") return VK_DELETE;
    if (code == "Home") return VK_HOME;
    if (code == "End") return VK_END;
    if (code == "PageUp") return VK_PRIOR;
    if (code == "PageDown") return VK_NEXT;
    if (code == "F1") return VK_F1;
    if (code == "F2") return VK_F2;
    if (code == "F3") return VK_F3;
    if (code == "F4") return VK_F4;
    if (code == "F5") return VK_F5;
    if (code == "F6") return VK_F6;
    if (code == "F7") return VK_F7;
    if (code == "F8") return VK_F8;
    if (code == "F9") return VK_F9;
    if (code == "F10") return VK_F10;
    if (code == "F11") return VK_F11;
    if (code == "F12") return VK_F12;
    if (code == "Minus") return VK_OEM_MINUS;
    if (code == "Equal") return VK_OEM_PLUS;
    if (code == "Period") return VK_OEM_PERIOD;
    if (code == "Comma") return VK_OEM_COMMA;
    return 0;
}

} // namespace

class InputInjectorWin : public IInputInjector {
public:
    bool mouseMove(double nx, double ny) override {
        int w = 0, h = 0;
        screenMetrics(w, h);
        if (w <= 0 || h <= 0) return false;
        INPUT in{};
        in.type = INPUT_MOUSE;
        // 精确归一化：先转目标像素，再按 MSDN 公式 x*65536/sw 映射（0-65535 近似式有 DPI 缩放下偏差）
        const int px = static_cast<int>(nx * (w - 1));
        const int py = static_cast<int>(ny * (h - 1));
        in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        in.mi.dx = static_cast<LONG>(px * 65536 / w);
        in.mi.dy = static_cast<LONG>(py * 65536 / h);
        return SendInput(1, &in, sizeof(in)) == 1;
    }

    bool mouseButton(int button, bool down) override {
        INPUT in{};
        in.type = INPUT_MOUSE;
        switch (button) {
        case 0: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
        case 1: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
        case 2: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
        default: return false;
        }
        return SendInput(1, &in, sizeof(in)) == 1;
    }

    bool mouseWheel(int dy) override {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = MOUSEEVENTF_WHEEL;
        in.mi.mouseData = static_cast<DWORD>(dy * 120); // WHEEL_DELTA
        return SendInput(1, &in, sizeof(in)) == 1;
    }

    bool key(const std::string& code, bool down) override {
        const WORD vk = codeToVk(code);
        if (vk == 0) return false;
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        return SendInput(1, &in, sizeof(in)) == 1;
    }
};

} // namespace echonode::platform

// 平台工厂（供 main 装配）
namespace echonode::platform {
std::shared_ptr<IInputInjector> createInputInjector() {
    return std::make_shared<InputInjectorWin>();
}
}

#endif // _WIN32
