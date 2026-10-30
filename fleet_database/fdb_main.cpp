#include <pqxx/pqxx>

#include "spdlog/spdlog.h"

int main() {
    try {
        pqxx::connection connection{"postgresql://fms:fms@127.0.0.1:5432/fms"};
        pqxx::work tx{connection};
        auto version = tx.query_value<std::string>("SELECT version()");
        spdlog::info("connected: {}", version);
        return 0;
    } catch (const std::exception& e) {
        spdlog::error("database: {}", e.what());
        return 1;
    }
}