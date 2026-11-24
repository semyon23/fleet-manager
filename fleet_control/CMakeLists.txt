set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS headers)
find_package(asio-grpc CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_executable(fleet_control fc_main.cpp
        services/Services.cpp
        api/HealthApi.cpp
        api/RobotApi.cpp
        fleet/FleetSync.cpp
        fleet/Availability.cpp
        fleet/AvailabilityTracker.cpp
        api/ApiCommon.cpp
        maps/Graph.cpp
        maps/MapStore.cpp
        api/MapApi.cpp
        routing/Router.cpp
        missions/Mission.cpp
        missions/MissionStore.cpp
        missions/ExecutionRecovery.cpp
        missions/CheckpointRecovery.cpp
        api/MissionApi.cpp
        assignment/MissionAssigner.cpp
        missions/MissionRunner.cpp
        api/RouteApi.cpp
        alarms/AlarmRules.cpp
        journal/RobotEvents.cpp
        journal/Journal.cpp
        alarms/AlarmEngine.cpp
        api/AlarmApi.cpp
        missions/RouteParts.cpp
        traffic/ResourceMap.cpp
        traffic/Reservations.cpp
        traffic/ReservationTable.cpp
        traffic/SippPlanner.cpp
        traffic/TrafficCoordinator.cpp
        traffic/BaseGuard.cpp
        traffic/BasePlanner.cpp
        missions/ExtensionCommand.cpp
        traffic/FixedRouteGate.cpp
        missions/ControlCommandJournal.cpp
        missions/ControlCommandJournal.hpp
)
target_include_directories(fleet_control PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fleet_control PRIVATE
        http_server
        api_json
        env_config
        proto_lib
        gRPC::grpc++
        asio-grpc::asio-grpc
        Boost::headers
        nlohmann_json::nlohmann_json
        spdlog::spdlog
)

if(FMS_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    include(Catch)

    add_executable(fleet_control_unit
            unit_tests/test_router.cpp
            unit_tests/test_mission.cpp
            unit_tests/test_alarm_rules.cpp
            unit_tests/test_robot_events.cpp
            unit_tests/test_route_parts.cpp
            unit_tests/test_traffic_resources.cpp
            unit_tests/test_reservation_table.cpp
            unit_tests/test_sipp.cpp
            unit_tests/test_traffic_coordinator.cpp
            unit_tests/test_base_guard.cpp
            unit_tests/test_base_planner.cpp
            unit_tests/test_extension_command.cpp
            unit_tests/test_execution_recovery.cpp
            missions/ExecutionRecovery.cpp
            unit_tests/test_fixed_route_gate.cpp
            routing/Router.cpp
            maps/Graph.cpp
            missions/Mission.cpp
            alarms/AlarmRules.cpp
            journal/RobotEvents.cpp
            missions/RouteParts.cpp
            traffic/ResourceMap.cpp
            traffic/Reservations.cpp
            traffic/ReservationTable.cpp
            traffic/SippPlanner.cpp
            traffic/TrafficCoordinator.cpp
            traffic/BaseGuard.cpp
            traffic/BasePlanner.cpp
            missions/ExtensionCommand.cpp
            traffic/FixedRouteGate.cpp

    )
    target_include_directories(fleet_control_unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(fleet_control_unit PRIVATE
            Catch2::Catch2WithMain
            Boost::headers
            nlohmann_json::nlohmann_json
            proto_lib
    )
    catch_discover_tests(fleet_control_unit)

    # Бенчмарк: не тест, в ctest не регистрируется, свой main. Запускать в Release
    add_executable(fleet_control_bench
            unit_tests/bench_router.cpp
            unit_tests/warehouse.cpp
            routing/Router.cpp
            maps/Graph.cpp



    )
    target_include_directories(fleet_control_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(fleet_control_bench PRIVATE
            Boost::headers
            nlohmann_json::nlohmann_json
    )
endif()