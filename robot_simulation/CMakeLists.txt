set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Boost REQUIRED COMPONENTS headers)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(behaviortree_cpp REQUIRED)

add_executable(robot_simulation src/rs_main.cpp
        src/RobotSimulator.cpp
        src/TreeNodes.cpp
        src/DiffDriveController.cpp
        src/RobotMessageRouter.cpp
../fleet_dispatch/src/MqttClientHandler.cpp)

target_include_directories(robot_simulation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(robot_simulation
        PRIVATE
        BT::behaviortree_cpp
        nlohmann_json::nlohmann_json
        Boost::headers
)

