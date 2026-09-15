#include "Dispatcher.hpp"

namespace echonode::executor {

void Dispatcher::add(std::unique_ptr<IExecutor> executor) {
    IExecutor* raw = executor.get();
    owned_.push_back(std::move(executor));
    for (const auto& action : raw->actions()) {
        executors_[action] = raw;
    }
}

protocol::TaskResult Dispatcher::dispatch(protocol::Task task) {
    auto it = executors_.find(task.action);
    if (it == executors_.end()) {
        return {task.taskId, false, {}, "action not implemented: " + task.action};
    }
    return it->second->execute(std::move(task));
}

} // namespace echonode::executor
