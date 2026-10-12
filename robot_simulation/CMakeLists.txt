find_package(nlohmann_json CONFIG REQUIRED)
find_package(behaviortree_cpp REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

add_executable(robot_simulation
        src/rs_main.cpp
        src/motion/RobotDriveController.cpp
        src/app/RobotMessageRouter.cpp
        src/behavior/action_nodes.cpp
        src/robot/RoboticVehicleSimulator.cpp
        src/vda/OrderManager.cpp
        src/vda/StatePublisher.cpp
        src/vda/factsheet_builder.cpp
        src/vda/ActionManager.cpp
        src/actions/ActionExecutor.cpp
        src/actions/PickDropActions.cpp
        src/actions/SimulatedAction.cpp
)

target_compile_definitions(robot_simulation PRIVATE
        ROBOT_TREE_XML="/home/ubuntu/fleet-manager/robot_simulation/trees/robot_tree.xml")

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
#// /home/ubuntu/fleet-manager/robot_simulation/trees/robot_tree.xml