// 输入注入：鼠标/键盘事件合成到目标系统，坐标用 0-1 归一化
#pragma once

#include <string>

namespace echonode::platform {

class IInputInjector {
public:
    virtual ~IInputInjector() = default;

    // 归一化坐标移动光标
    virtual bool mouseMove(double nx, double ny) = 0;
    // button: 0=左 1=中 2=右
    virtual bool mouseButton(int button, bool down) = 0;
    // dy: 正=向上滚
    virtual bool mouseWheel(int dy) = 0;
    // code: 浏览器 KeyboardEvent.code，实现负责映射
    virtual bool key(const std::string& code, bool down) = 0;
};

}
