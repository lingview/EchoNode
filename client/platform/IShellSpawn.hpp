// shell 执行：一次性命令 + 长驻交互式会话
#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "Error.hpp"

namespace echonode::platform {

// 不透明句柄，由具体实现解释，调用方不得解引用
using ShellHandle = void*;

class IShellSpawn {
public:
    virtual ~IShellSpawn() = default;

    // 执行一次性命令并返回全部输出（UTF-8）；启动失败抛 PlatformError
    virtual std::string runCommand(const std::string& command) = 0;

    // 启动长驻 shell（如 "cmd.exe" / "/bin/bash"），失败抛 PlatformError
    virtual ShellHandle spawn(const std::string& shellPath) = 0;

    // 向会话 stdin 写数据；会话已不可写时返回 false
    virtual bool write(ShellHandle handle, std::string_view data) = 0;

    // 非阻塞读 stdout：无新数据返回 false；读到 EOF 先交付剩余数据
    virtual bool tryRead(ShellHandle handle, std::string& out) = 0;

    // 会话进程是否仍存活
    virtual bool alive(ShellHandle handle) = 0;

    // 向会话进程组发中断（Ctrl+C 语义）；平台不支持时返回 false
    virtual bool interrupt(ShellHandle handle) { return false; }

    // 调整伪终端尺寸（列/行）；平台不支持时返回 false
    virtual bool resize(ShellHandle handle, int cols, int rows) { return false; }

    // 结束会话并释放句柄，调用后句柄失效
    virtual void terminate(ShellHandle handle) = 0;
};

}
