#include "SessionManager.hpp"

#include "util/Uuid.hpp"

namespace echonode::server {

std::string SessionManager::create(const std::string& user) {
    const std::string token = echonode::common::generateUuid();
    std::lock_guard<std::mutex> lk(mtx_);
    tokens_[token] = user;
    return token;
}

bool SessionManager::validate(const std::string& token) const {
    std::lock_guard<std::mutex> lk(mtx_);
    return tokens_.count(token) != 0;
}

} // namespace echonode::server
