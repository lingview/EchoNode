// 服务端 → 客户端方向的消息定义
#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace echonode::protocol {

// 任务下发，payload 由对应执行器自行解释
struct Task {
    std::string taskId; // 服务端生成的 UUID，客户端只透传
    std::string action; // 动作名
    nlohmann::json payload;
};

inline bool fromJson(const nlohmann::json& j, Task& out) {
    if (!j.is_object() || j.value("type", std::string{}) != "task") return false;
    out.taskId = j.value("taskId", std::string{});
    out.action = j.value("action", std::string{});
    if (auto it = j.find("payload"); it != j.end()) out.payload = *it;
    return !out.taskId.empty() && !out.action.empty();
}

} // namespace echonode::protocol
