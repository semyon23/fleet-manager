cmake_minimum_required(VERSION 3.15)
project(FleetManagerSystem CXX)

# Задаем глобальный стандарт C++ 23 для всех подпроектов
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Подавляем предупреждение FindBoost для CMake 3.30+
cmake_policy(SET CMP0167 OLD)

# Поиск глобальных зависимостей, общих для сервисов
find_package(Boost REQUIRED COMPONENTS headers)
find_package(gRPC CONFIG REQUIRED)
find_package(protobuf CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
# Подключаем папки с подпроектами
add_subdirectory(proto)
add_subdirectory(fleet_control)
add_subdirectory(fleet_dispatch)
add_subdirectory(robot_simulation)

#cmake_minimum_required(VERSION 3.15)
#cmake_policy(SET CMP0167 OLD)
#
#project(FleetManagerSystem CXX)
#
#set(CMAKE_CXX_STANDARD 17)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)
#
## Поиск глобальных зависимостей
#find_package(gRPC CONFIG REQUIRED)
#find_package(protobuf CONFIG REQUIRED)
#
#
#add_subdirectory(proto)
#add_subdirectory(fleet_control)
#add_subdirectory(fleet_dispatch)
#add_subdirectory(robot_simulation)

