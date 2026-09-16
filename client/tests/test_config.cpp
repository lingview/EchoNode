#include "core/Config.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int gFailed = 0;

void check(bool ok, const std::string& name) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
    if (!ok) ++gFailed;
}

struct Args {
    std::vector<std::string> store;
    std::vector<char*> ptrs;

    explicit Args(std::initializer_list<const char*> list) {
        store.emplace_back("echonode_agent");
        for (const char* s : list) store.emplace_back(s);
        for (auto& s : store) ptrs.push_back(s.data());
    }
    int argc() const { return static_cast<int>(ptrs.size()); }
    char** argv() { return ptrs.data(); }
};

std::string writeTemp(const std::string& name, const std::string& content) {
    const std::string path = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "/" + name;
    std::ofstream f(path, std::ios::trunc);
    f << content;
    f.close();
    return path;
}

}

int main() {
    std::cout << std::unitbuf;
    using echonode::core::Config;

    {
        Args a{"ws://10.0.0.1:8080/agent", "tok-cli"};
        Config c = Config::fromArgs(a.argc(), a.argv());
        check(c.valid && c.url == "ws://10.0.0.1:8080/agent" && c.token == "tok-cli",
              "命令行 url+token 位置参数");
    }

    {
        const std::string p = writeTemp("cfg_full.json",
            R"({"url":"ws://cfg-host:9000/agent","token":"tok-cfg",)"
            R"("heartbeatMs":5000,"backoffBaseMs":2000,"backoffCapMs":30000})");
        Args a{"--config", p.c_str()};
        Config c = Config::fromArgs(a.argc(), a.argv());
        check(c.valid && c.url == "ws://cfg-host:9000/agent" && c.token == "tok-cfg" &&
              c.heartbeatMs == 5000 && c.backoffBaseMs == 2000 && c.backoffCapMs == 30000,
              "config.json 加载全部字段");
    }

    {
        const std::string p = writeTemp("cfg_token.json", R"({"token":"tok-from-file"})");
        Args a{"ws://cli-url:1234/agent", "--config", p.c_str()};
        Config c = Config::fromArgs(a.argc(), a.argv());
        check(c.valid && c.url == "ws://cli-url:1234/agent" && c.token == "tok-from-file",
              "逐字段合并：命令行 url + 文件 token");
    }

    {
        const std::string p = writeTemp("cfg_override.json",
            R"({"url":"ws://file-url:1/agent","token":"file-tok","heartbeatMs":1111})");
        Args a{"ws://cli-url:2/agent", "cli-tok", "--heartbeat-ms", "2222",
               "--config", p.c_str()};
        Config c = Config::fromArgs(a.argc(), a.argv());
        check(c.valid && c.url == "ws://cli-url:2/agent" && c.token == "cli-tok" &&
              c.heartbeatMs == 2222,
              "命令行覆盖文件字段 (url/token/heartbeatMs)");
    }

    {
        const std::string p = writeTemp("cfg_partial.json", R"({"token":"only-token"})");
        Args a{"--config", p.c_str()};
        Config c = Config::fromArgs(a.argc(), a.argv());
        check(c.valid && c.token == "only-token" && c.heartbeatMs == 10000 &&
              c.backoffBaseMs == 1000 && c.backoffCapMs == 60000,
              "缺省字段回退内置默认值");
    }

    {
        const std::string p = writeTemp("cfg_broken.json", R"({"url": "ws://x", )");
        Args a{"--config", p.c_str()};
        Config c = Config::fromArgs(a.argc(), a.argv());
        check(!c.valid && !c.error.empty(), "JSON 语法错误被拒绝并报原因");
    }

    {
        const std::string p = writeTemp("cfg_badtype.json", R"({"heartbeatMs":"5000"})");
        Args a{"--config", p.c_str()};
        Config c = Config::fromArgs(a.argc(), a.argv());
        check(!c.valid && c.error.find("heartbeatMs") != std::string::npos,
              "字段类型错误被拒绝 (heartbeatMs 非整数)");
    }

    {
        const std::string p = writeTemp("cfg_array.json", R"([1,2,3])");
        Args a{"--config", p.c_str()};
        Config c = Config::fromArgs(a.argc(), a.argv());
        check(!c.valid && !c.error.empty(), "顶层非 JSON 对象被拒绝");
    }

    {
        Args a{"--config", "definitely_not_exist_cfg.json"};
        Config c = Config::fromArgs(a.argc(), a.argv());
        check(!c.valid && c.error.find("无法打开") != std::string::npos,
              "显式 --config 文件不存在报错");
    }

    {
        Args a{"http://bad-scheme:1234"};
        Config c = Config::fromArgs(a.argc(), a.argv());
        check(!c.valid && c.error.find("ws://") != std::string::npos,
              "非 ws:// 前缀 url 被拒绝");
    }

    {
        Args a{"--config"};
        Config c = Config::fromArgs(a.argc(), a.argv());
        check(!c.valid && !c.error.empty(), "--config 缺少路径参数被拒绝");
    }

    {
        Args a{"--no-such-flag"};
        Config c = Config::fromArgs(a.argc(), a.argv());
        check(!c.valid && c.error.find("未知参数") != std::string::npos,
              "未知参数被拒绝");
    }

    {
        Args a{"--heartbeat-ms", "abc"};
        Config c = Config::fromArgs(a.argc(), a.argv());
        check(!c.valid && c.error.find("整数") != std::string::npos,
              "--heartbeat-ms 非整数被拒绝");
    }

    for (const char* n : {"cfg_full.json", "cfg_token.json", "cfg_override.json",
                          "cfg_partial.json", "cfg_broken.json", "cfg_badtype.json",
                          "cfg_array.json"}) {
        const std::string p = std::string(getenv("TEMP") ? getenv("TEMP") : ".") + "/" + n;
        std::remove(p.c_str());
    }

    std::cout << (gFailed == 0 ? "== 全部通过 ==\n" : "== 存在失败项 ==\n");
    return gFailed == 0 ? 0 : 1;
}