// 操作员会话令牌：登录成功后签发，内存保存（server 重启即失效，需重新登录）
#pragma once
#include <map>
#include <mutex>
#include <string>

namespace echonode::server {

class SessionManager {
public:
    // 签发 token 并绑定用户
    std::string create(const std::string& user);
    bool validate(const std::string& token) const;

private:
    std::map<std::string, std::string> tokens_; // token → user
    mutable std::mutex mtx_;
};

} // namespace echonode::server
