#pragma once
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

namespace echonode::core {

struct Config {
    std::string url = "ws://127.0.0.1:9000";
    std::string token;
    int heartbeatMs = 10000;
    int backoffBaseMs = 1000;
    int backoffCapMs = 60000;

    bool valid = true;
    std::string error;
    bool console = false;
    bool deskStats = false;

    static Config fromArgs(int argc, char** argv) {
        Config cfg;

        std::string configPath = "config.json";
        bool configPathExplicit = false;
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--config") {
                if (i + 1 >= argc) {
                    cfg.valid = false;
                    cfg.error = "--config 缺少路径参数";
                    return cfg;
                }
                configPath = argv[++i];
                configPathExplicit = true;
            }
        }

        {
            std::ifstream in(configPath);
            if (in) {
                if (!cfg.loadFromJson(in, configPath)) return cfg;
            } else if (configPathExplicit) {
                cfg.valid = false;
                cfg.error = "指定的配置文件无法打开: " + configPath;
                return cfg;
            }
        }

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto next = [&](const char* name) -> std::string {
                if (i + 1 >= argc) {
                    cfg.valid = false;
                    cfg.error = std::string(name) + " 缺少参数值";
                    return {};
                }
                return argv[++i];
            };
            if (arg == "--config") {
                ++i;
            } else if (arg == "--heartbeat-ms") {
                const std::string v = next("--heartbeat-ms");
                if (!cfg.valid) return cfg;
                try {
                    cfg.heartbeatMs = std::stoi(v);
                } catch (const std::exception&) {
                    cfg.valid = false;
                    cfg.error = "--heartbeat-ms 需要整数，收到: " + v;
                    return cfg;
                }
            } else if (arg == "--console") {
                cfg.console = true;
            } else if (arg == "--desk-stats") {
                cfg.deskStats = true;
            } else if (arg.rfind("--", 0) == 0) {
                cfg.valid = false;
                cfg.error = "未知参数: " + arg;
                return cfg;
            } else if (!cfg.urlSet) {
                cfg.url = arg;
                cfg.urlSet = true;
            } else {
                cfg.token = arg;
            }
        }

        if (cfg.valid && cfg.url.rfind("ws://", 0) != 0 && cfg.url.rfind("wss://", 0) != 0) {
            cfg.valid = false;
            cfg.error = "url 仅支持 ws:// 或 wss:// 前缀";
        }
        return cfg;
    }

    static std::string usage() {
        return "用法: client [ws://host:port] [token] [--heartbeat-ms N] [--config path] [--console] [--desk-stats]\n"
               "未给位置参数时从 ./config.json 读取，字段: url/token/heartbeatMs/"
               "backoffBaseMs/backoffCapMs（均可选，命令行覆盖文件）；--console 显式弹窗看日志，"
               "--desk-stats 每 5s 打印远程桌面 [desk-stats] 分段耗时";
    }

private:
    bool urlSet = false;

    bool loadFromJson(std::istream& in, const std::string& path) {
        nlohmann::json j;
        try {
            in >> j;
        } catch (const std::exception& e) {
            valid = false;
            error = "配置文件解析失败(" + path + "): " + e.what();
            return false;
        }
        if (!j.is_object()) {
            valid = false;
            error = "配置文件格式错误(" + path + "): 顶层应为 JSON 对象";
            return false;
        }
        auto getStr = [&](const char* key, std::string& dst) -> bool {
            if (!j.contains(key) || j[key].is_null()) return true;
            if (!j[key].is_string()) {
                valid = false;
                error = std::string("配置字段 ") + key + " 应为字符串";
                return false;
            }
            dst = j[key].get<std::string>();
            return true;
        };
        auto getInt = [&](const char* key, int& dst) -> bool {
            if (!j.contains(key) || j[key].is_null()) return true;
            if (!j[key].is_number_integer()) {
                valid = false;
                error = std::string("配置字段 ") + key + " 应为整数";
                return false;
            }
            dst = j[key].get<int>();
            return true;
        };
        if (!getStr("url", url)) return false;
        if (!getStr("token", token)) return false;
        if (!getInt("heartbeatMs", heartbeatMs)) return false;
        if (!getInt("backoffBaseMs", backoffBaseMs)) return false;
        if (!getInt("backoffCapMs", backoffCapMs)) return false;
        return true;
    }
};

} // namespace echonode::core
