find_package(nlohmann_json CONFIG REQUIRED)
find_package(behaviortree_cpp REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

add_executable(robot_simulation
        src/rs_main.cpp
        src/RobotDriveController.cpp
        src/RobotMessageRouter.cpp
        src/action_nodes.cpp
        src/RoboticVehicleSimulator.cpp
        src/OrderManager.cpp
        src/StatePublisher.cpp
        src/factsheet_builder.cpp
        src/ActionManager.cpp
        src/ActionExecutor.cpp
        src/PickDropActions.cpp
        src/SimulatedAction.cpp
)

target_compile_definitions(robot_simulation PRIVATE
        ROBOT_TREE_XML="${CMAKE_CURRENT_SOURCE_DIR}/robot_tree.xml")

target_include_directories(robot_simulation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(robot_simulation
        PRIVATE
        mqtt_client
        yaml-cpp::yaml-cpp
        BT::behaviortree_cpp
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        Boost::headers
)
