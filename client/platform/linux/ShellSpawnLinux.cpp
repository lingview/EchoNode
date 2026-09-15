#include "IShellSpawn.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <memory>

namespace echonode::platform {
namespace {

struct LinuxSession {
    pid_t pid = -1;
    int fd = -1;      // pty 主端，读写共用
    bool eof = false;
};

class ShellSpawnLinux : public IShellSpawn {
public:
    ShellSpawnLinux() {
        // 管道写端关闭会触发 SIGPIPE 直接杀掉本进程，统一忽略改为检查返回值
        signal(SIGPIPE, SIG_IGN);
    }

    std::string runCommand(const std::string& command) override {
        FILE* pipe = popen((command + " 2>&1").c_str(), "r");
        if (!pipe) throw PlatformError("popen failed: " + command);
        std::string out;
        char buf[4096];
        size_t n = 0;
        while ((n = fread(buf, 1, sizeof(buf), pipe)) > 0) out.append(buf, n);
        pclose(pipe); // 退出码暂不使用，结果成败由上层按内容判定
        return out;
    }

    ShellHandle spawn(const std::string& shellPath) override {
        // 伪终端：vim/btop 等全屏程序依赖 TTY 检测、回显与行编辑由 pty 线路规则提供
        auto s = std::make_unique<LinuxSession>();
        winsize ws{}; // 初始 80x24，resize 后续经协议下发
        ws.ws_col = 80;
        ws.ws_row = 24;
        s->pid = forkpty(&s->fd, nullptr, nullptr, &ws);
        if (s->pid < 0) throw PlatformError("forkpty failed");
        if (s->pid == 0) {
            execl(shellPath.c_str(), shellPath.c_str(), static_cast<char*>(nullptr));
            _exit(127); // exec 失败只能退出子进程
        }
        // 读端非阻塞，tryRead 依赖 EAGAIN 判定"暂时没数据"
        fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
        return s.release();
    }

    bool write(ShellHandle handle, std::string_view data) override {
        auto* s = static_cast<LinuxSession*>(handle);
        // pty 线路规则（icrnl）自动处理回车转换，键入原样透传
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::write(s->fd, data.data() + off, data.size() - off);
            if (n < 0) return false; // EPIPE/EIO：会话已不可写
            off += static_cast<size_t>(n);
        }
        return true;
    }

    bool tryRead(ShellHandle handle, std::string& out) override {
        auto* s = static_cast<LinuxSession*>(handle);
        if (s->eof) return false;

        char buf[4096];
        bool got = false;
        while (true) {
            ssize_t n = ::read(s->fd, buf, sizeof(buf));
            if (n > 0) {
                out.append(buf, static_cast<size_t>(n));
                got = true;
                continue;
            }
            if (n == 0) { s->eof = true; return got; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) return got;
            if (errno == EINTR) continue;
            s->eof = true; // 其余错误视作会话结束
            return got;
        }
    }

    bool resize(ShellHandle handle, int cols, int rows) override {
        auto* s = static_cast<LinuxSession*>(handle);
        winsize ws{};
        ws.ws_col = static_cast<unsigned short>(cols);
        ws.ws_row = static_cast<unsigned short>(rows);
        return ioctl(s->fd, TIOCSWINSZ, &ws) == 0; // 内核自动向前台进程组发 SIGWINCH
    }

    bool alive(ShellHandle handle) override {
        auto* s = static_cast<LinuxSession*>(handle);
        int st = 0;
        return waitpid(s->pid, &st, WNOHANG) == 0; // >0 已退出（顺带收割），<0 异常同判
    }

    void terminate(ShellHandle handle) override {
        auto* s = static_cast<LinuxSession*>(handle);
        kill(s->pid, SIGKILL);
        int st = 0;
        waitpid(s->pid, &st, 0); // 阻塞收割，防僵尸进程
        close(s->fd);
        delete s;
    }
};

} // namespace

std::unique_ptr<IShellSpawn> createShellSpawn() {
    return std::make_unique<ShellSpawnLinux>();
}

} // namespace echonode::platform
