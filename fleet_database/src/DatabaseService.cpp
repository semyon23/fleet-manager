#include "../include/DatabaseService.hpp"
#include "../include/RobotTable.hpp"
#include "spdlog/spdlog.h"
#include <google/protobuf/util/time_util.h>
using Registry = dbv1::RobotRegistry::AsyncService;
using Storage = dbv1::DispatchStorage::AsyncService;

using CreateRobotRPC          = agrpc::ServerRPC<&Registry::RequestCreateRobot>;
using UpdateRobotRPC          = agrpc::ServerRPC<&Registry::RequestUpdateRobot>;
using SetRobotDeletingRPC     = agrpc::ServerRPC<&Registry::RequestSetRobotDeleting>;
using DeleteRobotRPC          = agrpc::ServerRPC<&Registry::RequestDeleteRobot>;
using GetRobotRPC             = agrpc::ServerRPC<&Registry::RequestGetRobot>;
using ListRobotsRPC           = agrpc::ServerRPC<&Registry::RequestListRobots>;
using GetFactsheetRPC         = agrpc::ServerRPC<&Registry::RequestGetFactsheet>;
using LoadDispatchSnapshotRPC = agrpc::ServerRPC<&Storage::RequestLoadDispatchSnapshot>;
using SaveRobotStatusRPC      = agrpc::ServerRPC<&Storage::RequestSaveRobotStatus>;
using SaveFactsheetRPC        = agrpc::ServerRPC<&Storage::RequestSaveFactsheet>;
using SaveMissionExecutionRPC = agrpc::ServerRPC<&Storage::RequestSaveMissionExecution>;
using ReserveHeaderIdsRPC     = agrpc::ServerRPC<&Storage::RequestReserveHeaderIds>;

const auto on_handler_done = [](std::exception_ptr error) {
    if (!error) return;
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        spdlog::error("[database_service] rpc handler failed: {}", e.what());
    }
};

grpc::Status database_error(const std::exception& e) {
    if (dynamic_cast<const pqxx::unique_violation*>(&e)) {
        return {grpc::StatusCode::ALREADY_EXISTS, "robot_id or manufacturer + serial_number already registered"};
    }

    if (dynamic_cast<const pqxx::foreign_key_violation*>(&e)) {
        return {grpc::StatusCode::NOT_FOUND, "robot not found"};
    }
    if (dynamic_cast<const pqxx::data_exception*>(&e)) {
        return {grpc::StatusCode::INVALID_ARGUMENT, e.what()};
    }

    if (dynamic_cast<const pqxx::broken_connection*>(&e)) {
        spdlog::error("[database_service] database connection lost: {}", e.what());
        return {grpc::StatusCode::UNAVAILABLE, "database unavailable, try again later"};
    }
    spdlog::error("[database_service] database error: {}", e.what());
    return {grpc::StatusCode::INTERNAL, "database error"};
}


grpc::Status unimplemented(const char* name) {
    return {grpc::StatusCode::UNIMPLEMENTED, std::string(name) + ": not implemented yet"};
}

DatabaseService::DatabaseService(agrpc::GrpcContext& grpc_context, Database& database)
    : grpc_context_(grpc_context), database_(database) {}

void DatabaseService::start() {

    agrpc::register_awaitable_rpc_handler<CreateRobotRPC>(
        grpc_context_, registry_,
        [this](CreateRobotRPC& rpc, CreateRobotRPC::Request& request) -> asio::awaitable<void> {
            CreateRobotRPC::Response response;
            grpc::Status status = co_await create_robot(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<UpdateRobotRPC>(
        grpc_context_, registry_,
        [this](UpdateRobotRPC& rpc, UpdateRobotRPC::Request& request) -> asio::awaitable<void> {
            UpdateRobotRPC::Response response;
            grpc::Status status = co_await update_robot(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<SetRobotDeletingRPC>(
        grpc_context_, registry_,
        [this](SetRobotDeletingRPC& rpc, SetRobotDeletingRPC::Request& request) -> asio::awaitable<void> {
            SetRobotDeletingRPC::Response response;
            grpc::Status status = co_await set_robot_deleting(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<DeleteRobotRPC>(
        grpc_context_, registry_,
        [this](DeleteRobotRPC& rpc, DeleteRobotRPC::Request& request) -> asio::awaitable<void> {
            DeleteRobotRPC::Response response;
            grpc::Status status = co_await delete_robot(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<GetRobotRPC>(
        grpc_context_, registry_,
        [this](GetRobotRPC& rpc, GetRobotRPC::Request& request) -> asio::awaitable<void> {
            GetRobotRPC::Response response;
            grpc::Status status = co_await get_robot(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<ListRobotsRPC>(
        grpc_context_, registry_,
        [this](ListRobotsRPC& rpc, ListRobotsRPC::Request& request) -> asio::awaitable<void> {
            ListRobotsRPC::Response response;
            grpc::Status status = co_await list_robots(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<GetFactsheetRPC>(
        grpc_context_, registry_,
        [this](GetFactsheetRPC& rpc, GetFactsheetRPC::Request& request) -> asio::awaitable<void> {
            GetFactsheetRPC::Response response;
            grpc::Status status = co_await get_factsheet(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    // ---------------------------------------------------------------- DispatchStorage

    agrpc::register_awaitable_rpc_handler<LoadDispatchSnapshotRPC>(
        grpc_context_, storage_,
        [this](LoadDispatchSnapshotRPC& rpc, LoadDispatchSnapshotRPC::Request& request) -> asio::awaitable<void> {
            LoadDispatchSnapshotRPC::Response response;
            grpc::Status status = co_await load_dispatch_snapshot(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<SaveRobotStatusRPC>(
        grpc_context_, storage_,
        [this](SaveRobotStatusRPC& rpc, SaveRobotStatusRPC::Request& request) -> asio::awaitable<void> {
            SaveRobotStatusRPC::Response response;
            grpc::Status status = co_await save_robot_status(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<SaveFactsheetRPC>(
        grpc_context_, storage_,
        [this](SaveFactsheetRPC& rpc, SaveFactsheetRPC::Request& request) -> asio::awaitable<void> {
            SaveFactsheetRPC::Response response;
            grpc::Status status = co_await save_factsheet(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<SaveMissionExecutionRPC>(
        grpc_context_, storage_,
        [this](SaveMissionExecutionRPC& rpc, SaveMissionExecutionRPC::Request& request) -> asio::awaitable<void> {
            SaveMissionExecutionRPC::Response response;
            grpc::Status status = co_await save_mission_execution(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<ReserveHeaderIdsRPC>(
        grpc_context_, storage_,
        [this](ReserveHeaderIdsRPC& rpc, ReserveHeaderIdsRPC::Request& request) -> asio::awaitable<void> {
            ReserveHeaderIdsRPC::Response response;
            grpc::Status status = co_await reserve_header_ids(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);
}

// ==================================================================== RobotRegistry (DB-2)

asio::awaitable<grpc::Status> DatabaseService::create_robot(const dbv1::CreateRobotRequest& request, dbv1::CreateRobotResponse& response) {
    const auto& spec = request.spec();
    if (spec.robot_id().empty() || spec.manufacturer().empty() || spec.serial_number().empty()) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT,
                               "robot_id, manufacturer and serial_number are required"};
    }
    try {
        dbv1::RobotSpec created;
        co_await database_.run([&](pqxx::work& tx) { created = insert_robot(tx, spec); });
        *response.mutable_spec() = created;
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    spdlog::info("[database_service] robot {} created", spec.robot_id());
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DatabaseService::update_robot(const dbv1::UpdateRobotRequest& request,
                                                            dbv1::UpdateRobotResponse& response) {
    if (request.spec().robot_id().empty()) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "robot_id is required"};
    }
    std::optional<dbv1::RobotSpec> updated;
    try {
        co_await database_.run([&](pqxx::work& tx) { updated = ::update_robot(tx, request.spec()); });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    if (!updated) {
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "robot " + request.spec().robot_id() + " not found"};
    }
    *response.mutable_spec() = *updated;
    co_return grpc::Status::OK;
}


asio::awaitable<grpc::Status> DatabaseService::set_robot_deleting(const dbv1::SetRobotDeletingRequest& request,
                                                                  dbv1::SetRobotDeletingResponse&) {
    bool found = false;
    try {
        co_await database_.run([&](pqxx::work& tx) { found = ::set_robot_deleting(tx, request.robot_id()); });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    if (!found) {
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "robot " + request.robot_id() + " not found"};
    }
    spdlog::info("[database_service] robot {} is pending delete", request.robot_id());
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DatabaseService::delete_robot(const dbv1::DeleteRobotRequest& request,
                                                            dbv1::DeleteRobotResponse&) {
    DeleteResult result{};
    try {
        co_await database_.run([&](pqxx::work& tx) { result = ::delete_robot(tx, request.robot_id()); });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    switch (result) {
    case DeleteResult::NotFound:
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "robot " + request.robot_id() + " not found"};
    case DeleteResult::NotDeleting:
        co_return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                               "robot " + request.robot_id() + " must be marked with SetRobotDeleting first"};
    case DeleteResult::Deleted:
        break;
    }
    spdlog::info("[database_service] robot {} deleted", request.robot_id());
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DatabaseService::get_robot(const dbv1::GetRobotRequest& request,
                                                         dbv1::GetRobotResponse& response) {
    std::optional<dbv1::RobotRecord> robot;
    try {
        co_await database_.run([&](pqxx::work& tx) { robot = select_robot(tx, request.robot_id()); });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    if (!robot) {
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "robot " + request.robot_id() + " not found"};
    }
    *response.mutable_robot() = std::move(*robot);
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DatabaseService::list_robots(const dbv1::ListRobotsRequest& request,
                                                           dbv1::ListRobotsResponse& response) {
    std::vector<dbv1::RobotRecord> robots;
    try {
        co_await database_.run([&](pqxx::work& tx) { robots = select_robots(tx, request.include_deleting()); });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    for (auto& robot : robots) {
        *response.add_robots() = std::move(robot);
    }
    co_return grpc::Status::OK;
}


asio::awaitable<grpc::Status> DatabaseService::get_factsheet(const dbv1::GetFactsheetRequest& request,
                                                             dbv1::GetFactsheetResponse& response)
{
    std::optional<dbv1::GetFactsheetResponse> factsheet;
    try {
        co_await database_.run([&](pqxx::work& tx) { factsheet = select_factsheet(tx, request.robot_id()); });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    if (!factsheet) {
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "no factsheet for robot " + request.robot_id()};
    }
    response = std::move(*factsheet);
    co_return grpc::Status::OK;
}
// ==================================================================== DispatchStorage (DB-3 и позже)

asio::awaitable<grpc::Status> DatabaseService::load_dispatch_snapshot(const dbv1::LoadDispatchSnapshotRequest&,
                                                                      dbv1::LoadDispatchSnapshotResponse&) {
    co_return unimplemented("LoadDispatchSnapshot");
}

asio::awaitable<grpc::Status> DatabaseService::save_robot_status(const dbv1::SaveRobotStatusRequest& request, dbv1::SaveRobotStatusResponse&) {
    const auto& status = request.status();
    if (status.robot_id().empty()) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "status.robot_id is required"};
    }
    try {
        co_await database_.run([&](pqxx::work& tx) { ::save_robot_status(tx, status); });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    co_return grpc::Status::OK;   // без лога: dispatch пишет статус часто
}

asio::awaitable<grpc::Status> DatabaseService::save_factsheet(const dbv1::SaveFactsheetRequest& request, dbv1::SaveFactsheetResponse&) {
    if (request.robot_id().empty()) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "robot_id is required"};
    }
    using google::protobuf::util::TimeUtil;
    const auto received_at = request.has_received_at() ? request.received_at() : TimeUtil::GetCurrentTime();
    try {
        co_await database_.run([&](pqxx::work& tx) {
            ::save_factsheet(tx, request.robot_id(), request.factsheet_json(),
                             TimeUtil::TimestampToMilliseconds(received_at));
        });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    spdlog::info("[database_service] factsheet of robot {} saved", request.robot_id());
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DatabaseService::save_mission_execution(const dbv1::SaveMissionExecutionRequest&,
                                                                      dbv1::SaveMissionExecutionResponse&) {
    co_return unimplemented("SaveMissionExecution");
}

asio::awaitable<grpc::Status> DatabaseService::reserve_header_ids(const dbv1::ReserveHeaderIdsRequest&,
                                                                  dbv1::ReserveHeaderIdsResponse&) {
    co_return unimplemented("ReserveHeaderIds");
}