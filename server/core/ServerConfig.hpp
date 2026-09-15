// 服务端配置：首次启动交互式安装（用户名密码+agent 令牌），落盘 server.json
#pragma once
#include <string>

namespace echonode::server {

struct ServerConfig {
    std::string operatorUser;
    std::string salt;
    std::string passwordHash; // sha256(salt + password)，绝不存明文
    std::string agentToken;   // agent 预共享令牌，安装时自动生成
    int httpPort = 8080;
    std::string bindAddr = "0.0.0.0";  // 反代部署时改 127.0.0.1 仅回环监听

    // path 不存在时进入终端交互式安装并保存；返回 false 表示安装被中止
    bool loadOrInstall(const std::string& path);

    bool verifyLogin(const std::string& user, const std::string& password) const;

    // 集成测试用：跳过交互直接构造完整配置
    static ServerConfig forTest(const std::string& user, const std::string& password,
                                const std::string& agentToken);
    bool save(const std::string& path) const;
};

}
