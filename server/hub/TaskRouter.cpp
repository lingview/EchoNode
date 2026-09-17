#include "TaskRouter.hpp"

#include "util/Sha256.hpp"
#include "util/Uuid.hpp"

#include <nlohmann/json.hpp>

namespace echonode::server {

namespace {

int64_t nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool hdlEq(const WsHdl& a, const WsHdl& b) {
    return !a.owner_before(b) && !b.owner_before(a);
}

} // namespace

TaskRouter::TaskRouter(AgentHub& hub, Database& db) : hub_(hub), db_(db) {
    hub.setOnAgentMessage([this](const std::string& agentId, const std::string& text) {
        onAgentMessage(agentId, text);
    });
    hub.setOnAgentBinary([this](const std::string& agentId, const void* data, size_t len) {
        onAgentBinary(agentId, data, len);
    });
}

void TaskRouter::setOperatorSink(OperatorSink* sink) {
    sink_ = sink;
}

void TaskRouter::submitTask(WsHdl op, const std::string& agentId,
                            const nlohmann::json& taskReq) {
    const std::string action = taskReq.value("action", std::string{});
    if (action.empty()) {
        if (sink_) sink_->sendToOperator(op, "{\"type\":\"task_result\",\"ok\":false,"
                                            "\"error\":\"action is required\"}");
        return;
    }
    const std::string taskId = echonode::common::generateUuid();
    const nlohmann::json payload = taskReq.value("payload", nlohmann::json::object());
    const std::string payloadStr = payload.dump();

    {
        std::lock_guard<std::mutex> lk(mtx_);
        tasks_[taskId] = {op, agentId, action};
    }
    if (action != "remote_input") {
        db_.exec("INSERT INTO tasks VALUES(?,?,?,?,?,'',?,0);",
                 {taskId, agentId, action, payloadStr, std::string("pending"),
                  nowSec()});
    }

    nlohmann::json task = {{"type", "task"},
                           {"taskId", taskId},
                           {"action", action},
                           {"payload", payload}};
    if (!hub_.sendTextToAgent(agentId, task.dump())) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            tasks_.erase(taskId);
        }
        db_.exec("UPDATE tasks SET status='failed', result='agent offline', finished_at=? "
                 "WHERE task_id=?;",
                 {nowSec(), taskId});
        if (sink_) {
            sink_->sendToOperator(
                op, nlohmann::json{{"type", "task_result"},
                                   {"taskId", taskId},
                                   {"ok", false},
                                   {"data", ""},
                                   {"error", "agent offline"}}
                        .dump());
        }
    }
}

void TaskRouter::onOperatorBinary(WsHdl op, const void* data, size_t len) {
    if (len < 16) return;
    const std::string taskId =
        echonode::common::uuidFromBytes(std::string(static_cast<const char*>(data), 16));
    std::string agentId;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = tasks_.find(taskId);
        if (it == tasks_.end()) return;
        agentId = it->second.agentId;
    }
    hub_.sendBinaryToAgent(agentId, data, len);
}

void TaskRouter::operatorGone(WsHdl op) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (hdlEq(it->second.op, op)) it = tasks_.erase(it);
        else ++it;
    }
    for (auto it = shells_.begin(); it != shells_.end();) {
        if (hdlEq(it->second.op, op)) {
            hub_.sendTextToAgent(it->second.agentId,
                                 nlohmann::json{{"type", "shell_data"},
                                                {"sessionId", it->first},
                                                {"data", ""},
                                                {"eof", true}}
                                     .dump());
            it = shells_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = streams_.begin(); it != streams_.end();) {
        if (hdlEq(it->second.op, op)) {
            hub_.sendTextToAgent(it->second.agentId,
                                 nlohmann::json{{"type", "task"},
                                                {"taskId", echonode::common::generateUuid()},
                                                {"action", "remote_stop"},
                                                {"payload", nlohmann::json::object()}}
                                     .dump());
            it = streams_.erase(it);
        } else {
            ++it;
        }
    }
}

void TaskRouter::onAgentMessage(const std::string& agentId, const std::string& text) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text);
    } catch (const std::exception&) {
        return;
    }
    const std::string type = j.value("type", std::string{});

    if (type == "shell_data") {
        const std::string sessionId = j.value("sessionId", std::string{});
        WsHdl op;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = shells_.find(sessionId);
            if (it == shells_.end()) return;
            op = it->second.op;
            if (j.value("eof", false)) {
                shells_.erase(it); // 会话结束，清理路由
            }
        }
        if (sink_) sink_->sendToOperator(op, text);
        return;
    }
    if (type == "file_ack") {
        const std::string taskId = j.value("taskId", std::string{});
        WsHdl op;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = tasks_.find(taskId);
            if (it != tasks_.end()) { op = it->second.op; found = true; }
        }
        if (found && sink_) sink_->sendToOperator(op, text);
        return;
    }
    if (type != "task_result") return;

    const std::string taskId = j.value("taskId", std::string{});
    WsHdl op;
    std::string action;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = tasks_.find(taskId);
        if (it == tasks_.end()) return;
        op = it->second.op;
        action = it->second.action;
        found = true;
    }
    if (!found) return;

    if (action == "shell_open" && j.value("ok", false)) {
        const std::string sessionId = j.value("data", std::string{});
        if (!sessionId.empty()) {
            std::lock_guard<std::mutex> lk(mtx_);
            shells_[sessionId] = {op, agentId};
        }
    }

    const bool ok = j.value("ok", false);
    const std::string result = j.value("data", std::string{});

    if (action == "remote_start" && ok) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = streams_.begin(); it != streams_.end();) {
            if (it->second.agentId == agentId) it = streams_.erase(it);
            else ++it;
        }
        streams_[taskId] = {op, agentId};
    }
    if (action == "remote_stop") {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = streams_.begin(); it != streams_.end();) {
            if (it->second.agentId == agentId) it = streams_.erase(it);
            else ++it;
        }
    }

    if (action == "file_upload" && ok) {
        bool isReady = false;
        try {
            isReady = nlohmann::json::parse(result).value("ready", false);
        } catch (const std::exception&) {
        }
        if (isReady) {
            j["action"] = action;
            j["agentId"] = agentId;
            if (sink_) sink_->sendToOperator(op, j.dump());
            return;
        }
    }

    db_.exec("UPDATE tasks SET status=?, result=?, finished_at=? WHERE task_id=?;",
             {std::string(ok ? "done" : "failed"), result, nowSec(), taskId});

    {
        std::lock_guard<std::mutex> lk(mtx_);
        tasks_.erase(taskId);
    }

    j["action"] = action;
    j["agentId"] = agentId;
    if (sink_) sink_->sendToOperator(op, j.dump());
}

void TaskRouter::onAgentBinary(const std::string& agentId, const void* data, size_t len) {
    if (len < 16) return;
    const std::string taskId =
        echonode::common::uuidFromBytes(std::string(static_cast<const char*>(data), 16));
    WsHdl op;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = tasks_.find(taskId);
        if (it != tasks_.end()) {
            op = it->second.op;
            found = true;
        } else {
            auto sit = streams_.find(taskId);
            if (sit != streams_.end()) {
                op = sit->second.op;
                found = true;
            }
        }
    }
    if (!found) return;
    if (sink_) sink_->sendBinaryToOperator(op, data, len);
}

void TaskRouter::onOperatorShellData(WsHdl op, const nlohmann::json& j) {
    const std::string sessionId = j.value("sessionId", std::string{});
    std::string agentId;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = shells_.find(sessionId);
        if (it == shells_.end()) return;
        agentId = it->second.agentId;
    }
    hub_.sendTextToAgent(agentId, j.dump());
}

void TaskRouter::onOperatorFileAck(WsHdl op, const nlohmann::json& j) {
    const std::string taskId = j.value("taskId", std::string{});
    std::string agentId;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = tasks_.find(taskId);
        if (it == tasks_.end()) return;
        agentId = it->second.agentId;
    }
    hub_.sendTextToAgent(agentId, j.dump());
}

void TaskRouter::onOperatorDeskStat(WsHdl op, const nlohmann::json& j) {
    const std::string taskId = j.value("taskId", std::string{});
    std::string agentId;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = streams_.find(taskId);
        if (it == streams_.end()) return;
        agentId = it->second.agentId;
    }
    hub_.sendTextToAgent(agentId, j.dump());
}

} // namespace echonode::server
