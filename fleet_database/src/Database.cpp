#include "../include/Database.hpp"

#include "../include/Database.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include "spdlog/spdlog.h"

Database::Database(const std::string& url)
    : connection_(url), work_(asio::make_work_guard(context_)) {
    spdlog::info("[database] connected to {} on {}", connection_.dbname(), connection_.hostname());
}

Database::~Database() {
    stop();
}

void Database::migrate(const std::string& migrations_dir) {
    {
        pqxx::work tx{connection_};
        tx.exec("CREATE TABLE IF NOT EXISTS schema_migrations ("
                "  version    TEXT PRIMARY KEY,"
                "  applied_at TIMESTAMPTZ NOT NULL DEFAULT now())");
        tx.commit();
    }

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(migrations_dir)) {
        if (entry.path().extension() == ".sql") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto& file : files) {
        std::string version = file.filename().string();
        pqxx::work tx{connection_};
        if (!tx.exec("SELECT 1 FROM schema_migrations WHERE version = $1", pqxx::params{version}).empty()) {
            continue;   // уже применена
        }

        std::ifstream in(file);
        std::stringstream sql;
        sql << in.rdbuf();

        tx.exec(sql.str());
        tx.exec("INSERT INTO schema_migrations (version) VALUES ($1)", pqxx::params{version});
        tx.commit();
        spdlog::info("[database] migration {} applied", version);
    }
}

void Database::start() {
    thread_ = std::thread([this] { context_.run(); });
}

void Database::stop() {
    work_.reset();
    if (thread_.joinable()) {
        thread_.join();
    }
}

asio::awaitable<void> Database::run(std::function<void(pqxx::work&)> query){
    co_await asio::co_spawn(
          context_,
          [this, query = std::move(query)]() -> asio::awaitable<void> {
              pqxx::work tx{connection_};
              query(tx);
              tx.commit();
              co_return;
          },
          asio::use_awaitable);
}