find_package(libpqxx CONFIG REQUIRED)
find_package(asio-grpc CONFIG REQUIRED)

add_executable(fleet_database
        fdb_main.cpp
        src/Database.cpp
        src/DatabaseService.cpp
        src/RobotTable.cpp
        src/RestApi.cpp
        src/MapTable.cpp
        src/MissionTable.cpp
        src/EventTable.cpp
        src/AlarmTable.cpp
        src/ControlCommandTable.cpp
)

target_include_directories(fleet_database PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(fleet_database PRIVATE
        http_server
        grpc_server
        env_config
        api_json
        proto_lib
        asio-grpc::asio-grpc
        Boost::headers
        libpqxx::pqxx
        spdlog::spdlog
)

target_compile_definitions(fleet_database PRIVATE FMS_MIGRATIONS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/migrations")

if(FMS_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)
    find_package(nlohmann_json CONFIG REQUIRED)
    include(Catch)

    add_executable(fleet_database_unit unit_tests/test_execution_order.cpp)
    target_include_directories(fleet_database_unit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(fleet_database_unit PRIVATE
            Catch2::Catch2WithMain
            proto_lib
            nlohmann_json::nlohmann_json
    )
    catch_discover_tests(fleet_database_unit)
endif()