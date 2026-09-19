set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS headers)
find_package(asio-grpc CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_executable(fleet_dispatch fd_main.cpp
        src/MqttClientHandler.cpp
        src/GrpcDispatchService.cpp
        src/DispatcherServer.cpp
        include/DispatcherServer.hpp
        include/types.hpp
        src/utils/RobotTopicManager.cpp
        include/utils/RobotTopicManager.hpp


)

target_link_libraries(fleet_dispatch
        PRIVATE
        asio-grpc::asio-grpc
        nlohmann_json::nlohmann_json
        proto_lib
        gRPC::grpc++_reflection
)

target_include_directories(fleet_dispatch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fleet_dispatch PRIVATE Boost::headers)