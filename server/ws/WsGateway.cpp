#include "WsGateway.hpp"

#include <asio.hpp>

#include <chrono>
#include <memory>
#include <mutex>

namespace echonode::server {

void WsGateway::addRoute(const std::string& path, WsRoute route) {
    routes_[path] = std::move(route);
}

void WsGateway::setHttpHandler(std::function<void(WsHdl)> handler) {
    httpHandler_ = std::move(handler);
}

std::string WsGateway::routePathOf(WsHdl hdl) {
    auto con = endpoint_.get_con_from_hdl(hdl);
    auto it = connRoute_.find(hdl);
    return it != connRoute_.end() ? it->second : std::string{};
}

void WsGateway::init(int port, const std::string& addr) {
    endpoint_.clear_access_channels(websocketpp::log::alevel::all);
    endpoint_.clear_error_channels(websocketpp::log::elevel::all);
    endpoint_.init_asio();
    endpoint_.set_reuse_addr(true);

    endpoint_.set_validate_handler([this](WsHdl hdl) {
        auto con = endpoint_.get_con_from_hdl(hdl);
        const std::string path = con->get_uri()->get_resource();
        if (!routes_.count(path)) return false;
        connRoute_[hdl] = path;
        return true;
    });

    endpoint_.set_open_handler([this](WsHdl hdl) {
        openConns_.insert(hdl);
        const std::string path = routePathOf(hdl);
        auto it = routes_.find(path);
        if (it != routes_.end() && it->second.onOpen) it->second.onOpen(hdl);
    });
    endpoint_.set_message_handler([this](WsHdl hdl, WsEndpoint::message_ptr msg) {
        const std::string path = routePathOf(hdl);
        auto it = routes_.find(path);
        if (it == routes_.end()) return;
        if (msg->get_opcode() == websocketpp::frame::opcode::binary) {
            const auto& payload = msg->get_payload();
            if (it->second.onBinary) it->second.onBinary(hdl, payload.data(), payload.size());
            return;
        }
        if (it->second.onText) it->second.onText(hdl, msg->get_payload());
    });
    auto disconnect = [this](WsHdl hdl) {
        const std::string path = routePathOf(hdl);
        openConns_.erase(hdl);
        connRoute_.erase(hdl);
        auto it = routes_.find(path);
        if (it != routes_.end() && it->second.onClose) it->second.onClose(hdl);
    };
    endpoint_.set_close_handler(disconnect);
    endpoint_.set_fail_handler(disconnect);
    endpoint_.set_http_handler([this](WsHdl hdl) {
        if (httpHandler_) httpHandler_(hdl);
    });

    if (addr == "0.0.0.0" || addr.empty()) {
        endpoint_.listen(static_cast<uint16_t>(port));
    } else {
        const asio::ip::tcp::endpoint ep(asio::ip::make_address(addr),
                                         static_cast<uint16_t>(port));
        endpoint_.listen(ep);
    }
    endpoint_.start_accept();
}

void WsGateway::run() {
    running_ = true;
    endpoint_.run();
}

void WsGateway::stop() {
    if (!running_.exchange(false)) return;
    endpoint_.stop_listening();
    websocketpp::lib::error_code ec;
    for (auto& hdl : openConns_) {
        endpoint_.close(hdl, websocketpp::close::status::going_away, "server stop", ec);
    }
}

void WsGateway::sendText(WsHdl hdl, const std::string& text) {
    auto payload = std::make_shared<std::string>(text);
    websocketpp::lib::asio::post(endpoint_.get_io_service(), [this, hdl, payload] {
        websocketpp::lib::error_code ec;
        endpoint_.send(hdl, *payload, websocketpp::frame::opcode::text, ec);
    });
}

void WsGateway::sendBinary(WsHdl hdl, const void* data, size_t len) {
    auto payload = std::make_shared<std::vector<uint8_t>>(
        static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + len);
    websocketpp::lib::asio::post(endpoint_.get_io_service(), [this, hdl, payload] {
        websocketpp::lib::error_code ec;
        endpoint_.send(hdl, payload->data(), payload->size(),
                       websocketpp::frame::opcode::binary, ec);
    });
}

void WsGateway::respondHttp(WsHdl hdl, int status, const std::string& contentType,
                            const std::string& body) {
    try {
    auto con = endpoint_.get_con_from_hdl(hdl);
    con->set_body(body);
    con->append_header("Content-Type", contentType);
    con->append_header("Cache-Control", "no-cache");
    con->set_status(static_cast<websocketpp::http::status_code::value>(status));
    } catch (const std::exception& e) {
    }
}

void WsGateway::closeConn(WsHdl hdl) {
    websocketpp::lib::asio::post(endpoint_.get_io_service(), [this, hdl] {
        websocketpp::lib::error_code ec;
        endpoint_.close(hdl, websocketpp::close::status::normal, "kick", ec);
    });
}

} // namespace echonode::server
