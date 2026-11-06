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
        maps/MapStore.hpp
        api/MapApi.cpp
        api/MapApi.hpp
        routing/Router.cpp
        routing/Router.hpp
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

# Тесты на C++ (Catch2): отдельная программа, в сервис не попадают.
# Собираются, только если FMS_BUILD_TESTS=ON (корневой CMakeLists.txt)
if(FMS_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    include(Catch)

    add_executable(fleet_control_unit
            unit_tests/test_router.cpp
            routing/Router.cpp
            maps/Graph.cpp
    )
    target_include_directories(fleet_control_unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(fleet_control_unit PRIVATE
            Catch2::Catch2WithMain
            Boost::headers
            nlohmann_json::nlohmann_json
    )
    catch_discover_tests(fleet_control_unit)
endif()