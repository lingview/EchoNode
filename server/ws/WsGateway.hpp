#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#include <thread>

#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

namespace echonode::server {

using WsHdl = websocketpp::connection_hdl;
using WsEndpoint = websocketpp::server<websocketpp::config::asio>;

struct HdlLess {
    bool operator()(const WsHdl& a, const WsHdl& b) const {
        return std::owner_less<websocketpp::connection_hdl>()(a, b);
    }
};

struct WsRoute {
    std::function<void(WsHdl)> onOpen;
    std::function<void(WsHdl, const std::string&)> onText;
    std::function<void(WsHdl, const void*, size_t)> onBinary;
    std::function<void(WsHdl)> onClose;
};

class WsGateway {
public:
    void addRoute(const std::string& path, WsRoute route);
    void setHttpHandler(std::function<void(WsHdl)> handler);

    void init(int port, const std::string& addr = "0.0.0.0");
    void run();
    void stop();

    void sendText(WsHdl hdl, const std::string& text);
    void sendBinary(WsHdl hdl, const void* data, size_t len);
    void closeConn(WsHdl hdl);
    void respondHttp(WsHdl hdl, int status, const std::string& contentType,
                     const std::string& body);
    std::shared_ptr<WsEndpoint::connection_type> endpoint(WsHdl hdl) {
        return endpoint_.get_con_from_hdl(hdl);
    }

private:
    std::string routePathOf(WsHdl hdl);

    WsEndpoint endpoint_;
    std::map<std::string, WsRoute> routes_;
    std::map<WsHdl, std::string, HdlLess> connRoute_;
    std::set<WsHdl, HdlLess> openConns_;
    std::function<void(WsHdl)> httpHandler_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace echonode::server
