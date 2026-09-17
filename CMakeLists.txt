cmake_minimum_required(VERSION 3.15)
project(FleetManagerSystem CXX)

set(CMAKE_CXX_STANDARD 17)

find_package(gRPC CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)

add_subdirectory(proto)
add_subdirectory(fleet_control)
add_subdirectory(fleet_dispatch)

