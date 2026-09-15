#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

struct sqlite3;

namespace echonode::server {

using SqlValue = std::variant<std::nullptr_t, int64_t, std::string>;

class Database {
public:
    explicit Database(const std::string& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool exec(const std::string& sql);
    void query(const std::string& sql,
               const std::function<void(const std::vector<std::string>&)>& rowFn);

    bool exec(const std::string& sql, const std::vector<SqlValue>& params);
    void query(const std::string& sql, const std::vector<SqlValue>& params,
               const std::function<void(const std::vector<std::string>&)>& rowFn);

private:
    sqlite3* db_ = nullptr;
    std::mutex mtx_;
};

} // namespace echonode::server
