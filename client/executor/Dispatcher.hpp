#pragma once
#include "IExecutor.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace echonode::executor {

class Dispatcher {
public:
    void add(std::unique_ptr<IExecutor> executor);

    protocol::TaskResult dispatch(protocol::Task task);

private:
    std::vector<std::unique_ptr<IExecutor>> owned_;
    std::map<std::string, IExecutor*> executors_;
};

}
