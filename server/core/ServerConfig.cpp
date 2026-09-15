#include "ServerConfig.hpp"

#include "util/Sha256.hpp"
#include "util/Uuid.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>

namespace echonode::server {

bool ServerConfig::loadOrInstall(const std::string& path) {
    std::ifstream in(path);
    if (in) {
        try {
            nlohmann::json j = nlohmann::json::parse(in);
            operatorUser = j.value("operatorUser", std::string{});
            salt = j.value("salt", std::string{});
            passwordHash = j.value("passwordHash", std::string{});
            agentToken = j.value("agentToken", std::string{});
            httpPort = j.value("httpPort", 8080);
            bindAddr = j.value("bindAddr", std::string{"0.0.0.0"});
            return !operatorUser.empty();
        } catch (const std::exception& e) {
            // 空文件/损坏配置不拒绝启动，回退到重新安装覆盖它
            std::cerr << "配置文件损坏或为空(" << e.what() << ")，重新进入安装\n";
        }
    }

    std::cout << "=== EchoNode Server 首次安装 ===\n";
    std::cout << "设置操作员用户名: ";
    std::getline(std::cin, operatorUser);
    if (operatorUser.empty()) {
        std::cerr << "用户名不能为空\n";
        return false;
    }
    while (true) {
        std::string password, confirm;
        // 注意：终端回显输入，v1 接受；后续可接 platform 层做不回显读取
        std::cout << "设置密码: ";
        std::getline(std::cin, password);
        std::cout << "确认密码: ";
        std::getline(std::cin, confirm);
        if (password.size() < 6) {
            std::cout << "密码至少 6 位\n";
            continue;
        }
        if (password != confirm) {
            std::cout << "两次输入不一致，重试\n";
            continue;
        }
        salt = echonode::common::generateUuid();
        passwordHash = echonode::common::sha256Hex((salt + password).data(),
                                                   salt.size() + password.size());
        break;
    }
    agentToken = echonode::common::generateUuid();
    std::cout << "请将以下 agent 令牌配置到客户端启动参数: " << agentToken << "\n";
    return save(path);
}

bool ServerConfig::verifyLogin(const std::string& user, const std::string& password) const {
    const std::string hash =
        echonode::common::sha256Hex((salt + password).data(), salt.size() + password.size());
    return user == operatorUser && hash == passwordHash;
}

ServerConfig ServerConfig::forTest(const std::string& user, const std::string& password,
                                   const std::string& token) {
    ServerConfig cfg;
    cfg.operatorUser = user;
    cfg.salt = echonode::common::generateUuid();
    cfg.passwordHash = echonode::common::sha256Hex((cfg.salt + password).data(),
                                                   cfg.salt.size() + password.size());
    cfg.agentToken = token;
    return cfg;
}

bool ServerConfig::save(const std::string& path) const {
    nlohmann::json j{{"operatorUser", operatorUser},
                     {"salt", salt},
                     {"passwordHash", passwordHash},
                     {"agentToken", agentToken},
                     {"httpPort", httpPort},
                     {"bindAddr", bindAddr}};
    std::ofstream out(path);
    out << j.dump(2);
    return static_cast<bool>(out);
}

} // namespace echonode::server
