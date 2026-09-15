#include "HttpApi.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

namespace echonode::server {

HttpApi::HttpApi(WsGateway& gw, AgentHub& hub, SessionManager& sessions, Database& db,
                 std::string webRoot,
                 std::function<bool(const std::string&, const std::string&)> verifyLogin)
    : gw_(gw), hub_(hub), sessions_(sessions), db_(db), webRoot_(std::move(webRoot)),
      verifyLogin_(std::move(verifyLogin)) {
    gw_.setHttpHandler([this](WsHdl hdl) { handle(hdl); });
}

void HttpApi::handle(WsHdl hdl) {
    try {
    auto con = gw_.endpoint(hdl);
    const std::string method = con->get_request().get_method();
    std::string path = con->get_request().get_uri();
    const auto qpos = path.find('?');
    if (qpos != std::string::npos) path.resize(qpos);

    if (method == "POST" && path == "/api/login") {
        nlohmann::json req;
        try {
            req = nlohmann::json::parse(con->get_request().get_body());
        } catch (const std::exception&) {
            respondJson(hdl, "{\"error\":\"bad request body\"}", 400);
            return;
        }
        if (!verifyLogin_(req.value("username", std::string{}),
                         req.value("password", std::string{}))) {
            respondJson(hdl, "{\"error\":\"invalid credentials\"}", 401);
            return;
        }
        respondJson(hdl, nlohmann::json{{"token", sessions_.create(req.value("username", std::string{}))}}.dump(),
                    200);
        return;
    }

    if (path.starts_with("/api/")) {
        if (!authorized(hdl)) {
            respondJson(hdl, "{\"error\":\"unauthorized\"}", 401);
            return;
        }
        if (method == "GET" && path == "/api/agents") {
            respondJson(hdl, hub_.agentsJson(), 200);
            return;
        }
        if (method == "GET" && path == "/api/tasks") {
            nlohmann::json arr = nlohmann::json::array();
            db_.query("SELECT task_id, agent_id, action, status, result, created_at, "
                      "finished_at FROM tasks ORDER BY created_at DESC LIMIT 200;",
                      [&](const std::vector<std::string>& row) {
                          arr.push_back({{"taskId", row[0]}, {"agentId", row[1]},
                                         {"action", row[2]}, {"status", row[3]},
                                         {"result", row[4]}, {"createdAt", row[5]},
                                         {"finishedAt", row[6]}});
                      });
            respondJson(hdl, arr.dump(), 200);
            return;
        }
        respondJson(hdl, "{\"error\":\"not found\"}", 404);
        return;
    }

    if (method == "GET") {
        std::string rel = path == "/" ? "/index.html" : path;
        respondFile(hdl, webRoot_ + rel);
        return;
    }
    respondJson(hdl, "{\"error\":\"method not allowed\"}", 405);
    } catch (const std::exception& e) {
        respondJson(hdl, "{\"error\":\"internal\"}", 500);
    }
}

bool HttpApi::authorized(WsHdl hdl) const {
    auto con = gw_.endpoint(hdl);
    const std::string auth = con->get_request().get_header("Authorization");
    const std::string prefix = "Bearer ";
    if (!auth.starts_with(prefix)) return false;
    return sessions_.validate(auth.substr(prefix.size()));
}

void HttpApi::respondJson(WsHdl hdl, const std::string& body, int status) {
    gw_.respondHttp(hdl, status, "application/json", body);
}

void HttpApi::respondFile(WsHdl hdl, const std::string& path) {
    // 防止目录穿越用..等情况
    if (path.find("..") != std::string::npos) {
        gw_.respondHttp(hdl, 403, "text/plain", "forbidden");
        return;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        gw_.respondHttp(hdl, 404, "text/plain", "not found");
        return;
    }
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    gw_.respondHttp(hdl, 200, mimeType(path), body);
}

std::string HttpApi::mimeType(const std::string& path) {
    if (path.ends_with(".html")) return "text/html; charset=utf-8";
    if (path.ends_with(".css")) return "text/css; charset=utf-8";
    if (path.ends_with(".js")) return "application/javascript";
    if (path.ends_with(".json")) return "application/json";
    if (path.ends_with(".png")) return "image/png";
    if (path.ends_with(".ico")) return "image/x-icon";
    return "application/octet-stream";
}

} // namespace echonode::server
