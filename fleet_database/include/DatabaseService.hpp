#ifndef FLEETMANAGERSYSTEM_DATABASESERVICE_HPP
#define FLEETMANAGERSYSTEM_DATABASESERVICE_HPP

#pragma once

#include <agrpc/asio_grpc.hpp>
#include <boost/asio/awaitable.hpp>

#include "Database.hpp"
#include "fleet_database.grpc.pb.h"

namespace dbv1 = fms::database::v1;

class DatabaseService {
public:
    DatabaseService(agrpc::GrpcContext& grpc_context, Database& database);

    grpc::Service& registry_service() { return registry_; }
    grpc::Service& storage_service() { return storage_; }
    grpc::Service& control_service() { return control_; }
    void start();

private:
    // RobotRegistry
    asio::awaitable<grpc::Status> create_robot(const dbv1::CreateRobotRequest& request, dbv1::CreateRobotResponse& response);
    asio::awaitable<grpc::Status> update_robot(const dbv1::UpdateRobotRequest& request, dbv1::UpdateRobotResponse& response);
    asio::awaitable<grpc::Status> set_robot_deleting(const dbv1::SetRobotDeletingRequest& request, dbv1::SetRobotDeletingResponse& response);
    asio::awaitable<grpc::Status> delete_robot(const dbv1::DeleteRobotRequest& request, dbv1::DeleteRobotResponse& response);
    asio::awaitable<grpc::Status> get_robot(const dbv1::GetRobotRequest& request, dbv1::GetRobotResponse& response);
    asio::awaitable<grpc::Status> list_robots(const dbv1::ListRobotsRequest& request, dbv1::ListRobotsResponse& response);
    asio::awaitable<grpc::Status> get_factsheet(const dbv1::GetFactsheetRequest& request, dbv1::GetFactsheetResponse& response);

    // DispatchStorage
    asio::awaitable<grpc::Status> load_dispatch_snapshot(const dbv1::LoadDispatchSnapshotRequest& request, dbv1::LoadDispatchSnapshotResponse& response);
    asio::awaitable<grpc::Status> save_robot_status(const dbv1::SaveRobotStatusRequest& request, dbv1::SaveRobotStatusResponse& response);
    asio::awaitable<grpc::Status> save_factsheet(const dbv1::SaveFactsheetRequest& request, dbv1::SaveFactsheetResponse& response);
    asio::awaitable<grpc::Status> save_mission_execution(const dbv1::SaveMissionExecutionRequest& request, dbv1::SaveMissionExecutionResponse& response);
    asio::awaitable<grpc::Status> reserve_header_ids(const dbv1::ReserveHeaderIdsRequest& request, dbv1::ReserveHeaderIdsResponse& response);

    asio::awaitable<grpc::Status> save_robot_availability(const dbv1::SaveRobotAvailabilityRequest& request,dbv1::SaveRobotAvailabilityResponse& response);

    // ControlStorage (M-1)
    asio::awaitable<grpc::Status> create_map(const dbv1::CreateMapRequest& request, dbv1::CreateMapResponse& response);
    asio::awaitable<grpc::Status> update_map(const dbv1::UpdateMapRequest& request, dbv1::UpdateMapResponse& response);
    asio::awaitable<grpc::Status> delete_map(const dbv1::DeleteMapRequest& request, dbv1::DeleteMapResponse& response);
    asio::awaitable<grpc::Status> get_map(const dbv1::GetMapRequest& request, dbv1::GetMapResponse& response);
    asio::awaitable<grpc::Status> list_maps(const dbv1::ListMapsRequest& request, dbv1::ListMapsResponse& response);

    // ControlStorage (M-3)
    asio::awaitable<grpc::Status> save_mission(const dbv1::SaveMissionRequest& request, dbv1::SaveMissionResponse& response);
    asio::awaitable<grpc::Status> get_mission(const dbv1::GetMissionRequest& request, dbv1::GetMissionResponse& response);
    asio::awaitable<grpc::Status> list_active_missions(const dbv1::ListActiveMissionsRequest& request, dbv1::ListActiveMissionsResponse& response);

    agrpc::GrpcContext& grpc_context_;
    Database& database_;
    dbv1::RobotRegistry::AsyncService registry_;
    dbv1::DispatchStorage::AsyncService storage_;
    dbv1::ControlStorage::AsyncService control_;
};
#endif //FLEETMANAGERSYSTEM_DATABASESERVICE_HPP
