#include "../include/DatabaseService.hpp"
#include "../include/RobotTable.hpp"
#include "../include/MapTable.hpp"
#include "../include/MissionTable.hpp"
#include "spdlog/spdlog.h"
#include <google/protobuf/util/time_util.h>
#include <nlohmann/json.hpp>
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
using SaveRobotAvailabilityRPC = agrpc::ServerRPC<&Registry::RequestSaveRobotAvailability>;

using Control = dbv1::ControlStorage::AsyncService;

using CreateMapRPC = agrpc::ServerRPC<&Control::RequestCreateMap>;
using UpdateMapRPC = agrpc::ServerRPC<&Control::RequestUpdateMap>;
using DeleteMapRPC = agrpc::ServerRPC<&Control::RequestDeleteMap>;
using GetMapRPC    = agrpc::ServerRPC<&Control::RequestGetMap>;
using ListMapsRPC  = agrpc::ServerRPC<&Control::RequestListMaps>;

using SaveMissionRPC        = agrpc::ServerRPC<&Control::RequestSaveMission>;
using GetMissionRPC         = agrpc::ServerRPC<&Control::RequestGetMission>;
using ListActiveMissionsRPC = agrpc::ServerRPC<&Control::RequestListActiveMissions>;

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

// Событие миссии (c3): пусто — всё верно, иначе что не так. Ошибка здесь — ошибка в control
std::string event_error(const dbv1::MissionEvent& event) {
    if (event.type() != "STATE" && event.type() != "STEP" && event.type() != "ACTION") {
        return "unknown type '" + event.type() + "'";
    }
    if (!event.has_at()) {
        return "at is required";
    }
    // details склеиваются с полями строки (||) — должен быть объект, иначе получится массив
    if (!event.details_json().empty() && !nlohmann::json::parse(event.details_json(), nullptr, false).is_object()) {
        return "details_json must be a JSON object";
    }
    return "";
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

    agrpc::register_awaitable_rpc_handler<SaveRobotAvailabilityRPC>(
        grpc_context_, registry_,
        [this](SaveRobotAvailabilityRPC& rpc, SaveRobotAvailabilityRPC::Request& request) -> asio::awaitable<void> {
            SaveRobotAvailabilityRPC::Response response;
            grpc::Status status = co_await save_robot_availability(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    // ---------------------------------------------------------------- ControlStorage (M-1)

    agrpc::register_awaitable_rpc_handler<CreateMapRPC>(
        grpc_context_, control_,
        [this](CreateMapRPC& rpc, CreateMapRPC::Request& request) -> asio::awaitable<void> {
            CreateMapRPC::Response response;
            grpc::Status status = co_await create_map(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<UpdateMapRPC>(
        grpc_context_, control_,
        [this](UpdateMapRPC& rpc, UpdateMapRPC::Request& request) -> asio::awaitable<void> {
            UpdateMapRPC::Response response;
            grpc::Status status = co_await update_map(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<DeleteMapRPC>(
        grpc_context_, control_,
        [this](DeleteMapRPC& rpc, DeleteMapRPC::Request& request) -> asio::awaitable<void> {
            DeleteMapRPC::Response response;
            grpc::Status status = co_await delete_map(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<GetMapRPC>(
        grpc_context_, control_,
        [this](GetMapRPC& rpc, GetMapRPC::Request& request) -> asio::awaitable<void> {
            GetMapRPC::Response response;
            grpc::Status status = co_await get_map(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<ListMapsRPC>(
        grpc_context_, control_,
        [this](ListMapsRPC& rpc, ListMapsRPC::Request& request) -> asio::awaitable<void> {
            ListMapsRPC::Response response;
            grpc::Status status = co_await list_maps(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    // ---------------------------------------------------------------- ControlStorage (M-3)

    agrpc::register_awaitable_rpc_handler<SaveMissionRPC>(
        grpc_context_, control_,
        [this](SaveMissionRPC& rpc, SaveMissionRPC::Request& request) -> asio::awaitable<void> {
            SaveMissionRPC::Response response;
            grpc::Status status = co_await save_mission(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<GetMissionRPC>(
        grpc_context_, control_,
        [this](GetMissionRPC& rpc, GetMissionRPC::Request& request) -> asio::awaitable<void> {
            GetMissionRPC::Response response;
            grpc::Status status = co_await get_mission(request, response);
            co_await rpc.finish(response, status);
        },
        on_handler_done);

    agrpc::register_awaitable_rpc_handler<ListActiveMissionsRPC>(
        grpc_context_, control_,
        [this](ListActiveMissionsRPC& rpc, ListActiveMissionsRPC::Request& request) -> asio::awaitable<void> {
            ListActiveMissionsRPC::Response response;
            grpc::Status status = co_await list_active_missions(request, response);
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

asio::awaitable<grpc::Status> DatabaseService::save_robot_availability(const dbv1::SaveRobotAvailabilityRequest& request, dbv1::SaveRobotAvailabilityResponse&) {
    if (request.robot_id().empty() || request.availability().state() == dbv1::AVAILABILITY_STATE_UNSPECIFIED) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "robot_id and availability.state are required"};
    }
    try {
        co_await database_.run([&](pqxx::work& tx) {
            ::save_robot_availability(tx, request.robot_id(), request.availability());
        });
    } catch (const std::exception& e) {
        co_return database_error(e);   // нет робота — foreign_key_violation → NOT_FOUND
    }
    co_return grpc::Status::OK;        // без лога: control пишет при каждом изменении
}

// ==================================================================== ControlStorage (M-1)

asio::awaitable<grpc::Status> DatabaseService::create_map(const dbv1::CreateMapRequest& request,
                                                          dbv1::CreateMapResponse& response) {
    const auto& map = request.map();
    if (map.map_id().empty() || map.body_json().empty()) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "map_id and body_json are required"};
    }
    try {
        dbv1::MapRecord created;
        co_await database_.run([&](pqxx::work& tx) { created = insert_map(tx, map); });
        *response.mutable_map() = std::move(created);
    } catch (const pqxx::unique_violation&) {
        // database_error говорит про робота — у карты свой текст
        co_return grpc::Status{grpc::StatusCode::ALREADY_EXISTS, "map " + map.map_id() + " already exists"};
    } catch (const std::exception& e) {
        co_return database_error(e);   // body_json не JSON — data_exception → INVALID_ARGUMENT
    }
    spdlog::info("[database_service] map {} created", map.map_id());
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DatabaseService::update_map(const dbv1::UpdateMapRequest& request,
                                                          dbv1::UpdateMapResponse& response) {
    const auto& map = request.map();
    if (map.map_id().empty() || map.body_json().empty()) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT, "map_id and body_json are required"};
    }
    std::optional<std::string> new_image;
    if (request.has_new_image()) {
        new_image = request.new_image();
    }
    MapUpdateResult result{};
    try {
        co_await database_.run([&](pqxx::work& tx) {
            result = ::update_map(tx, map, request.expected_version(), new_image);
        });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    switch (result.result) {
    case MapUpdateResult::NotFound:
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "map " + map.map_id() + " not found"};
    case MapUpdateResult::VersionConflict:
        co_return grpc::Status{grpc::StatusCode::ABORTED,
                               "map " + map.map_id() + " was saved by someone else, expected version " +
                               std::to_string(request.expected_version())};
    case MapUpdateResult::Updated:
        break;
    }
    *response.mutable_map() = std::move(result.map);
    spdlog::info("[database_service] map {} saved, version {}", map.map_id(), response.map().version());
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DatabaseService::delete_map(const dbv1::DeleteMapRequest& request,
                                                          dbv1::DeleteMapResponse&) {
    bool found = false;
    try {
        co_await database_.run([&](pqxx::work& tx) { found = ::delete_map(tx, request.map_id()); });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    if (!found) {
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "map " + request.map_id() + " not found"};
    }
    spdlog::info("[database_service] map {} deleted", request.map_id());
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DatabaseService::get_map(const dbv1::GetMapRequest& request,
                                                       dbv1::GetMapResponse& response) {
    std::optional<dbv1::MapRecord> map;
    try {
        co_await database_.run([&](pqxx::work& tx) { map = select_map(tx, request.map_id()); });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    if (!map) {
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "map " + request.map_id() + " not found"};
    }
    *response.mutable_map() = std::move(*map);
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DatabaseService::list_maps(const dbv1::ListMapsRequest&,
                                                         dbv1::ListMapsResponse& response) {
    std::vector<dbv1::MapRecord> maps;
    try {
        co_await database_.run([&](pqxx::work& tx) { maps = select_maps(tx); });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    for (auto& map : maps) {
        *response.add_maps() = std::move(map);
    }
    co_return grpc::Status::OK;
}

// ==================================================================== ControlStorage (M-3)

asio::awaitable<grpc::Status> DatabaseService::save_mission(const dbv1::SaveMissionRequest& request,
                                                            dbv1::SaveMissionResponse&) {
    const auto& mission = request.mission();
    if (mission.mission_id().empty() || mission.map_id().empty() || mission.body_json().empty() ||
        !mission.has_created_at() || !mission.has_updated_at()) {
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT,
                               "mission_id, map_id, body_json, created_at and updated_at are required"};
        }
    for (int i = 0; i < request.events_size(); ++i) {
        if (std::string error = event_error(request.events(i)); !error.empty()) {
            co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT,
                                   "mission " + mission.mission_id() + ": event " + std::to_string(i) + ": " + error};
        }
    }
    try {
        // Одна транзакция: миссия, потом её события (внешний ключ). Ошибка — ни того, ни другого
        co_await database_.run([&](pqxx::work& tx) {
            ::save_mission(tx, mission);
            insert_mission_events(tx, mission.mission_id(), request.events());
        });
    } catch (const pqxx::check_violation&) {
        // CHECK в таблице: ошибка в control, а не в данных оператора
        co_return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT,
                               "mission " + mission.mission_id() + ": invalid state '" + mission.state() +
                               "' or priority " + std::to_string(mission.priority())};
    } catch (const std::exception& e) {
        co_return database_error(e);   // body_json не JSON — data_exception → INVALID_ARGUMENT
    }
    co_return grpc::Status::OK;        // без лога: control пишет при каждом изменении миссии
}

asio::awaitable<grpc::Status> DatabaseService::get_mission(const dbv1::GetMissionRequest& request,
                                                           dbv1::GetMissionResponse& response) {
    std::optional<dbv1::MissionRecord> mission;
    try {
        co_await database_.run([&](pqxx::work& tx) { mission = select_mission(tx, request.mission_id()); });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    if (!mission) {
        co_return grpc::Status{grpc::StatusCode::NOT_FOUND, "mission " + request.mission_id() + " not found"};
    }
    *response.mutable_mission() = std::move(*mission);
    co_return grpc::Status::OK;
}

asio::awaitable<grpc::Status> DatabaseService::list_active_missions(const dbv1::ListActiveMissionsRequest&,
                                                                    dbv1::ListActiveMissionsResponse& response) {
    std::vector<dbv1::MissionRecord> missions;
    try {
        co_await database_.run([&](pqxx::work& tx) { missions = select_active_missions(tx); });
    } catch (const std::exception& e) {
        co_return database_error(e);
    }
    for (auto& mission : missions) {
        *response.add_missions() = std::move(mission);
    }
    co_return grpc::Status::OK;
}
