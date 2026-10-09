cmake_minimum_required(VERSION 3.15)
project(FleetManagerSystem CXX)
#-DCMAKE_TOOLCHAIN_FILE=/home/ubuntu/.vcpkg-clion/vcpkg/scripts/buildsystems/vcpkg.cmake  // home
#-DCMAKE_TOOLCHAIN_FILE=/home/ubuntu/vcpkg/scripts/buildsystems/vcpkg.cmake               // work
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

cmake_policy(SET CMP0167 OLD)


find_package(Boost REQUIRED COMPONENTS headers)
find_package(gRPC CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

#add_subdirectory(shared/mqtt_client)

add_subdirectory(proto)
add_subdirectory(fleet_control)
add_subdirectory(fleet_dispatch)
add_subdirectory(robot_simulation)

add_library(mqtt_client STATIC
        shared/src/mqtt_client.cpp
        shared/include/mqtt_client.hpp
)

target_include_directories(mqtt_client PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/fleet_dispatch/include
)

# Привязываем Boost, так как MqttClientHandler использует Asio/Mqtt5
target_link_libraries(mqtt_client PRIVATE Boost::headers)