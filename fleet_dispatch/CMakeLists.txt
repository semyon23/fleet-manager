set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS headers)
find_package(asio-grpc CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(behaviortree_cpp REQUIRED)

add_executable(fleet_dispatch fd_main.cpp
        src/MqttClientHandler.cpp
        src/GrpcDispatchService.cpp
        src/DispatcherServer.cpp
        include/DispatcherServer.hpp
        include/types.hpp
        src/controllers/RobotTopicManager.cpp
        include/controllers/RobotTopicManager.hpp
        src/controllers/RobotController.cpp
        include/controllers/RobotController.hpp


)

target_link_libraries(fleet_dispatch
        PRIVATE
        BT::behaviortree_cpp
        asio-grpc::asio-grpc
        nlohmann_json::nlohmann_json
        proto_lib
        gRPC::grpc++_reflection
)

target_include_directories(fleet_dispatch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fleet_dispatch PRIVATE Boost::headers)