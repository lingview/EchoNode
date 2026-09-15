// 客户端 → 服务端方向的消息定义
#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace echonode::protocol {

// 上线注册，连接后第一条消息，token 校验失败服务端断开
struct Register {
    std::string hostname;
    std::string osName;
    std::string osVersion;
    std::string userName;
    std::string arch;
    std::string token;
};

// 应用层心跳
struct Heartbeat {
    int64_t ts; // 毫秒级 UTC 时间戳
};

// 任务执行结果，taskId 透传服务端生成的值
struct TaskResult {
    std::string taskId;
    bool ok;
    std::string data;  // 成功时的结果内容
    std::string error; // 失败时的错误描述
};

// 交互式 shell 会话数据流，双向同构
struct ShellData {
    std::string sessionId;
    std::string data;
    bool eof; // 会话结束，对端收到后清理
};

inline nlohmann::json toJson(const Register& m) {
    return nlohmann::json{
        {"type", "register"}, {"hostname", m.hostname}, {"osName", m.osName},
        {"osVersion", m.osVersion}, {"userName", m.userName},
        {"arch", m.arch}, {"token", m.token}};
}

inline nlohmann::json toJson(const Heartbeat& m) {
    return nlohmann::json{{"type", "heartbeat"}, {"ts", m.ts}};
}

inline nlohmann::json toJson(const TaskResult& m) {
    return nlohmann::json{{"type", "task_result"}, {"taskId", m.taskId},
                          {"ok", m.ok}, {"data", m.data}, {"error", m.error}};
}

inline nlohmann::json toJson(const ShellData& m) {
    return nlohmann::json{{"type", "shell_data"}, {"sessionId", m.sessionId},
                          {"data", m.data}, {"eof", m.eof}};
}

} // namespace echonode::protocol
