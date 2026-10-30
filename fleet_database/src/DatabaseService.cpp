//
// Created by ubuntu on 2026-10-30.
//

#include "../include/DatabaseService.hpp"
#include "DatabaseService.hpp"

#include "spdlog/spdlog.h"

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

grpc::Status unimplemented(const char* name) {
    return {grpc::StatusCode::UNIMPLEMENTED, std::string(name) + ": not implemented yet"};
}

DatabaseService::DatabaseService(agrpc::GrpcContext& grpc_context, Database& database)
    : grpc_context_(grpc_context), database_(database) {}

void DatabaseService::start() {
    // ---------------------------------------------------------------- RobotRegistry

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

asio::awaitable<grpc::Status> DatabaseService::create_robot(const dbv1::CreateRobotRequest&, dbv1::CreateRobotResponse&) {
    co_return unimplemented("CreateRobot");
}

asio::awaitable<grpc::Status> DatabaseService::update_robot(const dbv1::UpdateRobotRequest&, dbv1::UpdateRobotResponse&) {
    co_return unimplemented("UpdateRobot");
}

asio::awaitable<grpc::Status> DatabaseService::set_robot_deleting(const dbv1::SetRobotDeletingRequest&,
                                                                  dbv1::SetRobotDeletingResponse&) {
    co_return unimplemented("SetRobotDeleting");
}

asio::awaitable<grpc::Status> DatabaseService::delete_robot(const dbv1::DeleteRobotRequest&, dbv1::DeleteRobotResponse&) {
    co_return unimplemented("DeleteRobot");
}

asio::awaitable<grpc::Status> DatabaseService::get_robot(const dbv1::GetRobotRequest&, dbv1::GetRobotResponse&) {
    co_return unimplemented("GetRobot");
}

asio::awaitable<grpc::Status> DatabaseService::list_robots(const dbv1::ListRobotsRequest&, dbv1::ListRobotsResponse&) {
    co_return unimplemented("ListRobots");
}

asio::awaitable<grpc::Status> DatabaseService::get_factsheet(const dbv1::GetFactsheetRequest&, dbv1::GetFactsheetResponse&) {
    co_return unimplemented("GetFactsheet");
}

// ==================================================================== DispatchStorage (DB-3 и позже)

asio::awaitable<grpc::Status> DatabaseService::load_dispatch_snapshot(const dbv1::LoadDispatchSnapshotRequest&,
                                                                      dbv1::LoadDispatchSnapshotResponse&) {
    co_return unimplemented("LoadDispatchSnapshot");
}

asio::awaitable<grpc::Status> DatabaseService::save_robot_status(const dbv1::SaveRobotStatusRequest&,
                                                                 dbv1::SaveRobotStatusResponse&) {
    co_return unimplemented("SaveRobotStatus");
}

asio::awaitable<grpc::Status> DatabaseService::save_factsheet(const dbv1::SaveFactsheetRequest&, dbv1::SaveFactsheetResponse&) {
    co_return unimplemented("SaveFactsheet");
}

asio::awaitable<grpc::Status> DatabaseService::save_mission_execution(const dbv1::SaveMissionExecutionRequest&,
                                                                      dbv1::SaveMissionExecutionResponse&) {
    co_return unimplemented("SaveMissionExecution");
}

asio::awaitable<grpc::Status> DatabaseService::reserve_header_ids(const dbv1::ReserveHeaderIdsRequest&,
                                                                  dbv1::ReserveHeaderIdsResponse&) {
    co_return unimplemented("ReserveHeaderIds");
}