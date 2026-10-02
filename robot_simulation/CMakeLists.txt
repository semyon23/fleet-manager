find_package(nlohmann_json CONFIG REQUIRED)
find_package(behaviortree_cpp REQUIRED)
find_package(yaml-cpp CONFIG REQUIRED)

# Создаем исполняемый файл подпроекта
add_executable(robot_simulation
        src/rs_main.cpp
        src/RobotSimulator.cpp
        src/RobotDriveController.cpp
        src/RobotMessageRouter.cpp
        ../fleet_dispatch/src/MqttClientHandler.cpp
        src/action_nodes.cpp
        src/RobotBehaviorImpl.cpp
        src/RoboticVehicleSimulator.cpp
        include/RoboticVehicleSimulator.hpp

)

# Передаем путь к XML файлу дерева через дефайн компилятора
target_compile_definitions(robot_simulation PRIVATE
        ROBOT_TREE_XML="${CMAKE_CURRENT_SOURCE_DIR}/robot_tree.xml")

# Инклуды
target_include_directories(robot_simulation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Линковка библиотек
target_link_libraries(robot_simulation
        PRIVATE
        yaml-cpp::yaml-cpp
        BT::behaviortree_cpp
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        Boost::headers
)
#set(CMAKE_CXX_STANDARD 23)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)
#
#find_package(Boost REQUIRED COMPONENTS headers)
#find_package(nlohmann_json CONFIG REQUIRED)
#find_package(behaviortree_cpp REQUIRED)
#find_package(yaml-cpp CONFIG REQUIRED)
#
#add_executable(robot_simulation src/rs_main.cpp
#        src/RobotSimulator.cpp
#        src/TreeNodes.cpp
#        src/RobotDriveController.cpp
#        src/RobotMessageRouter.cpp
#../fleet_dispatch/src/MqttClientHandler.cpp
#)
#
#target_include_directories(robot_simulation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
#target_compile_definitions(robot_simulation PRIVATE
#        ROBOT_TREE_XML="${CMAKE_CURRENT_SOURCE_DIR}/robot_tree.xml")
#target_link_libraries(robot_simulation
#        PRIVATE
#        yaml-cpp::yaml-cpp
#        BT::behaviortree_cpp
#        nlohmann_json::nlohmann_json
#        Boost::headers
#)

#-DCMAKE_TOOLCHAIN_FILE=/home/ubuntu/vcpkg/scripts/buildsystems/vcpkg.cmake