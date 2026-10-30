set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS headers)
find_package(asio-grpc CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(libpqxx CONFIG REQUIRED)

add_executable(fleet_database fdb_main.cpp

)

target_link_libraries(fleet_dispatch
        PRIVATE
        vda5050
        spdlog::spdlog
        libpqxx::pqxx
        asio-grpc::asio-grpc
        nlohmann_json::nlohmann_json
        proto_lib
        gRPC::grpc++_reflection
)

target_include_directories(fleet_database PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fleet_database PRIVATE Boost::headers)
target_link_libraries(fleet_database PRIVATE
        libpqxx::pqxx
        spdlog::spdlog
        Boost::headers
)