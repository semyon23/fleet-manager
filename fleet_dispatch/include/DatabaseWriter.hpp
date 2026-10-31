#ifndef FLEETMANAGERSYSTEM_DATABASEWRITER_HPP
#define FLEETMANAGERSYSTEM_DATABASEWRITER_HPP

#pragma once

#include <agrpc/asio_grpc.hpp>
#include <grpcpp/grpcpp.h>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/experimental/channel.hpp>
#include <map>
#include <memory>
#include <set>
#include <string>

#include "fleet_database.grpc.pb.h"

namespace asio = boost::asio;

class DatabaseWriter {
public:
    DatabaseWriter(agrpc::GrpcContext& grpc_context, asio::io_context& io_context, const std::string& address);

    void start();
    void stop();

    // Запомнить статус робота; run() запишет его. Предыдущий, если ещё не записан, заменяется:
    // в базе нужен только последний.
    void save_status(const fms::v1::RobotStatus& status);

    // Записать factsheet сейчас. false — не записан (причина уже в логе).
    asio::awaitable<bool> save_factsheet(const std::string& robot_id, const std::string& json, const google::protobuf::Timestamp& received_at);

private:
    asio::awaitable<void> run();
    asio::awaitable<grpc::Status> send_status(const fms::v1::RobotStatus& status);
    void report(const std::string& robot_id, const grpc::Status& result);

    agrpc::GrpcContext& grpc_context_;
    asio::io_context& io_context_;
    std::unique_ptr<fms::database::v1::DispatchStorage::Stub> stub_;

    using Signal = asio::experimental::channel<void(boost::system::error_code)>;
    Signal wakeup_;   // «в pending_ есть статус»; вместимость 1 — сигнал либо есть,
    std::map<std::string, fms::v1::RobotStatus> pending_;   // robot_id → последний незаписанный статус
    asio::steady_timer retry_timer_;   // пауза перед повтором, когда база недоступна
    std::set<std::string> not_registered_;   // о них уже предупредили: робота нет в базе
    bool available_ = true;                  // для лога: база пропала / вернулась
    bool stopped_ = false;
};


#endif //FLEETMANAGERSYSTEM_DATABASEWRITER_HPP
