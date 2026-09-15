#include "ProcessExecutor.hpp"

#include "../platform/Error.hpp"

#include <nlohmann/json.hpp>

namespace echonode::executor {

protocol::TaskResult ProcessExecutor::execute(protocol::Task task) {
    try {
        if (task.action == "ps_list") {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& p : ops_->listProcesses()) {
                arr.push_back({{"pid", p.pid}, {"name", p.name}, {"user", p.user}});
            }
            return {task.taskId, true, arr.dump(), {}};
        }
        if (task.action == "ps_kill") {
            const auto pid = task.payload.value("pid", 0);
            if (pid == 0) {
                return {task.taskId, false, {}, "payload.pid is required"};
            }
            if (!ops_->killProcess(static_cast<uint32_t>(pid))) {
                return {task.taskId, false, {}, "kill failed: no permission or no such process"};
            }
            return {task.taskId, true, "ok", {}};
        }
        return {task.taskId, false, {}, "unknown process action"};
    } catch (const platform::PlatformError& e) {
        return {task.taskId, false, {}, e.what()};
    }
}

} // namespace echonode::executor
