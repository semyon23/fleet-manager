set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS headers)
find_package(asio-grpc CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_executable(fleet_control fc_main.cpp

)

target_link_libraries(fleet_control
        PRIVATE
        http_server
        grpc_server
        env_config
        asio-grpc::asio-grpc
        nlohmann_json::nlohmann_json
        proto_lib
        gRPC::grpc++_reflection
)

target_include_directories(fleet_control PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fleet_control PRIVATE Boost::headers)