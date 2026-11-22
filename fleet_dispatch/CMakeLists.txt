set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS headers)
find_package(asio-grpc CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(behaviortree_cpp REQUIRED)

add_executable(fleet_dispatch fd_main.cpp
        src/DispatchService.cpp
        src/RobotSession.cpp
        src/FleetRegistry.cpp
        src/RobotMessageRouter.cpp
        src/RobotStatusBuilder.cpp
        src/RobotStatusHub.cpp
        src/OrderBuilder.cpp
        src/OrderTracker.cpp
        src/ActiveMission.cpp
        src/MissionStatusHub.cpp
        src/DatabaseWriter.cpp
        src/MoveRecovery.cpp
)

target_link_libraries(fleet_dispatch
        PRIVATE
        vda5050
        spdlog::spdlog
        grpc_server
        mqtt_client
        env_config
        BT::behaviortree_cpp
        asio-grpc::asio-grpc
        nlohmann_json::nlohmann_json
        proto_lib
        gRPC::grpc++_reflection
)

target_include_directories(fleet_dispatch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fleet_dispatch PRIVATE Boost::headers)

if(FMS_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    include(Catch)

    add_executable(fleet_dispatch_unit
            unit_tests/test_move_recovery.cpp
            src/MoveRecovery.cpp
            src/OrderBuilder.cpp
            unit_tests/test_startup_inventory.cpp
            unit_tests/test_probe_queue.cpp
    )
    target_link_libraries(fleet_dispatch_unit PRIVATE
            Catch2::Catch2WithMain
            proto_lib
            vda5050
            nlohmann_json::nlohmann_json
    )
    catch_discover_tests(fleet_dispatch_unit)
endif()