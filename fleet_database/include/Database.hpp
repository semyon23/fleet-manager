
#ifndef FLEETMANAGERSYSTEM_DATABASE_HPP
#define FLEETMANAGERSYSTEM_DATABASE_HPP

#pragma once

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/awaitable.hpp>
#include <functional>
#include <pqxx/pqxx>
#include <string>
#include <thread>

namespace asio = boost::asio;

class Database {
public:
    explicit Database(const std::string& url);
    ~Database();
    void migrate(const std::string& migrations_dir);

    void start();
    void stop();
    asio::awaitable<void> run(std::function<void(pqxx::work&)> query);
    asio::io_context& context() { return context_; }
    pqxx::connection& connection() { return connection_; }

private:
    pqxx::connection connection_;
    asio::io_context context_;
    asio::executor_work_guard<asio::io_context::executor_type> work_;
    std::thread thread_;
};


#endif //FLEETMANAGERSYSTEM_DATABASE_HPP
