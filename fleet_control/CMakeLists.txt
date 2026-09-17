set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS headers)
find_package(asio-grpc CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
add_executable(fleet_control fsm_main.cpp
        src/api/HttpSession.cpp
        src/api/ApiController.cpp
        src/api/RobotApiHandler.cpp
        src/core/RobotsService.cpp
        src/core/MissionService.cpp
        src/core/FleetDispatchClient.cpp
        include/core/FleetDispatchClient.hpp
        src/api/WebSocketBroadcaster.cpp
        include/api/WebSocketBroadcaster.hpp


)

target_link_libraries(fleet_control
        PRIVATE

        asio-grpc::asio-grpc
        nlohmann_json::nlohmann_json
        proto_lib
        gRPC::grpc++_reflection
)

target_include_directories(fleet_control PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fleet_control PRIVATE Boost::headers)