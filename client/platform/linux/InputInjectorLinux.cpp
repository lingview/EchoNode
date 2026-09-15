// XTest 实现：X11 会话下合成鼠标/键盘事件（需链接 -lXtst）
#ifndef _WIN32
#include "IInputInjector.hpp"
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <cstdlib>
#include <memory>
#include <map>
#include <mutex>
#include <string>

namespace echonode::platform {
namespace {

Display* ensureDisplay() {
    static Display* dpy = XOpenDisplay(nullptr);
    return dpy;
}

// 浏览器 KeyboardEvent.code → X11 keysym（常用键覆盖）
KeySym codeToKeysym(const std::string& code) {
    if (code.size() == 4 && code.rfind("Key", 0) == 0) {
        const char c = code[3];
        if (c >= 'A' && c <= 'Z') return XK_a + (c - 'A');
    }
    if (code.size() == 6 && code.rfind("Digit", 0) == 0) {
        const char c = code[5];
        if (c >= '0' && c <= '9') return XK_0 + (c - '0');
    }
    if (code == "Backspace") return XK_BackSpace;
    if (code == "Tab") return XK_Tab;
    if (code == "Enter") return XK_Return;
    if (code == "ShiftLeft") return XK_Shift_L;
    if (code == "ShiftRight") return XK_Shift_R;
    if (code == "ControlLeft") return XK_Control_L;
    if (code == "ControlRight") return XK_Control_R;
    if (code == "AltLeft") return XK_Alt_L;
    if (code == "AltRight") return XK_Alt_R;
    if (code == "Escape") return XK_Escape;
    if (code == "Space") return XK_space;
    if (code == "ArrowUp") return XK_Up;
    if (code == "ArrowDown") return XK_Down;
    if (code == "ArrowLeft") return XK_Left;
    if (code == "ArrowRight") return XK_Right;
    if (code == "Delete") return XK_Delete;
    if (code == "Home") return XK_Home;
    if (code == "End") return XK_End;
    if (code == "PageUp") return XK_Prior;
    if (code == "PageDown") return XK_Next;
    if (code.rfind("F", 0) == 0 && code.size() >= 2) {
        const int n = std::atoi(code.c_str() + 1);
        if (n >= 1 && n <= 12) return XK_F1 + (n - 1);
    }
    if (code == "Minus") return XK_minus;
    if (code == "Equal") return XK_equal;
    if (code == "Period") return XK_period;
    if (code == "Comma") return XK_comma;
    return 0;
}

} // namespace

class InputInjectorLinux : public IInputInjector {
public:
    bool mouseMove(double nx, double ny) override {
        Display* dpy = ensureDisplay();
        if (!dpy) return false;
        const int w = DisplayWidth(dpy, DefaultScreen(dpy));
        const int h = DisplayHeight(dpy, DefaultScreen(dpy));
        XTestFakeMotionEvent(dpy, DefaultScreen(dpy),
                             static_cast<int>(nx * (w - 1)),
                             static_cast<int>(ny * (h - 1)), CurrentTime);
        XFlush(dpy);
        return true;
    }

    bool mouseButton(int button, bool down) override {
        Display* dpy = ensureDisplay();
        if (!dpy) return false;
        // XTest 按钮：1=左 2=中 3=右，滚轮 4/5
        const unsigned b = static_cast<unsigned>(button + 1);
        XTestFakeButtonEvent(dpy, b, down ? True : False, CurrentTime);
        XFlush(dpy);
        return true;
    }

    bool mouseWheel(int dy) override {
        Display* dpy = ensureDisplay();
        if (!dpy) return false;
        const unsigned up = dy > 0 ? 4 : 5;
        const int times = std::abs(dy);
        for (int i = 0; i < times; ++i) {
            XTestFakeButtonEvent(dpy, up, True, CurrentTime);
            XTestFakeButtonEvent(dpy, up, False, CurrentTime);
        }
        XFlush(dpy);
        return true;
    }

    bool key(const std::string& code, bool down) override {
        Display* dpy = ensureDisplay();
        if (!dpy) return false;
        const KeySym sym = codeToKeysym(code);
        if (sym == 0) return false;
        const KeyCode kc = XKeysymToKeycode(dpy, sym);
        if (kc == 0) return false;
        XTestFakeKeyEvent(dpy, kc, down ? True : False, CurrentTime);
        XFlush(dpy);
        return true;
    }
};

} // namespace echonode::platform

namespace echonode::platform {
std::shared_ptr<IInputInjector> createInputInjector() {
    return std::make_shared<InputInjectorLinux>();
}
}

#endif // !_WIN32
