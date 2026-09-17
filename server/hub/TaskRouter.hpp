// 任务路由：taskId ↔ 发起 operator，agent 结果回传与 binary 双向转发，任务落库
// 另管理交互式 shell 会话的 sessionId 路由（shell_open 建立连接，shell_data 双向转发）
#pragma once
#include "../db/Database.hpp"
#include "../hub/AgentHub.hpp"

#include <map>
#include <mutex>
#include <string>

namespace echonode::server {

class TaskRouter {
public:
    // operator 侧发送能力，由 OperatorHub 实现注入（避免环依赖）
    struct OperatorSink {
        virtual void sendToOperator(WsHdl hdl, const std::string& text) = 0;
        virtual void sendBinaryToOperator(WsHdl hdl, const void* data, size_t len) = 0;
        virtual ~OperatorSink() = default;
    };

    TaskRouter(AgentHub& hub, Database& db);
    void setOperatorSink(OperatorSink* sink);
    void submitTask(WsHdl op, const std::string& agentId,
                    const nlohmann::json& taskReq);
    void onOperatorBinary(WsHdl op, const void* data, size_t len);
    void operatorGone(WsHdl op);
    void onAgentMessage(const std::string& agentId, const std::string& text);
    void onAgentBinary(const std::string& agentId, const void* data, size_t len);

    void onOperatorShellData(WsHdl op, const nlohmann::json& j);
    void onOperatorFileAck(WsHdl op, const nlohmann::json& j);
    void onOperatorDeskStat(WsHdl op, const nlohmann::json& j);

private:
    struct Pending {
        WsHdl op;
        std::string agentId;
        std::string action;
    };
    struct ShellSession {
        WsHdl op;
        std::string agentId;
    };

    AgentHub& hub_;
    Database& db_;
    OperatorSink* sink_ = nullptr;
    std::mutex mtx_;
    std::map<std::string, Pending> tasks_;          // taskId 发起者
    std::map<std::string, ShellSession> shells_;    // sessionId 归属
    std::map<std::string, ShellSession> streams_;   // 桌面流 taskId 归属（result 后仍保留）
};

} // namespace echonode::server
