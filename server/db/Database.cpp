#include "Database.hpp"

#include <sqlite3.h>

namespace echonode::server {

namespace {
void bindValue(sqlite3_stmt* stmt, int idx, const SqlValue& v) {
    if (std::holds_alternative<std::nullptr_t>(v)) {
        sqlite3_bind_null(stmt, idx);
    } else if (std::holds_alternative<int64_t>(v)) {
        sqlite3_bind_int64(stmt, idx, std::get<int64_t>(v));
    } else {
        const auto& s = std::get<std::string>(v);
        sqlite3_bind_text(stmt, idx, s.c_str(), static_cast<int>(s.size()),
                          SQLITE_TRANSIENT);
    }
}
} // namespace

Database::Database(const std::string& path) {
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) return;

    // WAL 提升并发读写，外键本工程未用
    exec("PRAGMA journal_mode=WAL;");
    exec("CREATE TABLE IF NOT EXISTS tasks("
         "task_id TEXT PRIMARY KEY, agent_id TEXT, action TEXT,"
         "payload TEXT, status TEXT, result TEXT,"
         "created_at INTEGER, finished_at INTEGER);");
    exec("CREATE TABLE IF NOT EXISTS agents("
         "agent_id TEXT PRIMARY KEY, hostname TEXT, os_name TEXT, os_version TEXT,"
         "user_name TEXT, arch TEXT, first_seen INTEGER, last_seen INTEGER);");
}

Database::~Database() {
    if (db_) sqlite3_close(db_);
}

bool Database::exec(const std::string& sql) {
    std::lock_guard<std::mutex> lk(mtx_);
    char* err = nullptr;
    bool ok = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
    if (err) sqlite3_free(err);
    return ok;
}

void Database::query(
    const std::string& sql,
    const std::function<void(const std::vector<std::string>&)>& rowFn) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int cols = sqlite3_column_count(stmt);
        std::vector<std::string> row;
        row.reserve(cols);
        for (int i = 0; i < cols; ++i) {
            const auto* text = sqlite3_column_text(stmt, i);
            row.emplace_back(text ? reinterpret_cast<const char*>(text) : "");
        }
        rowFn(row);
    }
    sqlite3_finalize(stmt);
}

bool Database::exec(const std::string& sql, const std::vector<SqlValue>& params) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return false;
    for (size_t i = 0; i < params.size(); ++i)
        bindValue(stmt, static_cast<int>(i) + 1, params[i]);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

void Database::query(
    const std::string& sql, const std::vector<SqlValue>& params,
    const std::function<void(const std::vector<std::string>&)>& rowFn) {
    std::lock_guard<std::mutex> lk(mtx_);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) return;
    for (size_t i = 0; i < params.size(); ++i)
        bindValue(stmt, static_cast<int>(i) + 1, params[i]);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const int cols = sqlite3_column_count(stmt);
        std::vector<std::string> row;
        row.reserve(cols);
        for (int i = 0; i < cols; ++i) {
            const auto* text = sqlite3_column_text(stmt, i);
            row.emplace_back(text ? reinterpret_cast<const char*>(text) : "");
        }
        rowFn(row);
    }
    sqlite3_finalize(stmt);
}

} // namespace echonode::server
