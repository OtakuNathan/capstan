#pragma once
#include <capstan/transport/io.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
struct sqlite3;
namespace capstan { namespace durable {
enum class commit_outcome {committed,not_committed,unknown};
struct commit_result {commit_outcome outcome;int code;};
struct record {std::string key;bytes value;};
enum class fault_point {before_commit,after_commit};
using fault_hook=void(*)(fault_point); // Tests may terminate a child at this boundary.
// One local database. Transactions atomically publish the complete record batch.
// Neither SQLite statements nor caller buffers escape an operation's mutex.
class sqlite_store {
    sqlite3* db_=nullptr;
    std::string path_;
    mutable std::mutex mutex_;
    bool failed_=false;
    int error_=0;
    std::vector<int> locks_;
    std::vector<std::string> actors_;
    bool exec(const char* sql) noexcept;
public:
    explicit sqlite_store(const std::string& path);
    ~sqlite_store() noexcept;
    sqlite_store(const sqlite_store&)=delete;
    sqlite_store& operator=(const sqlite_store&)=delete;
    bool healthy() const noexcept;
    int error() const noexcept;
    bool claim_actor(const std::string& actor) noexcept;
    bool get(const std::string& key,bytes& value,bool& found) noexcept;
    bool list(const std::string& prefix,std::vector<record>& rows,std::size_t limit=4096) noexcept;
    commit_result commit(const std::vector<record>& batch,fault_hook hook=nullptr) noexcept;
};
}}
