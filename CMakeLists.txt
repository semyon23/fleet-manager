cmake_minimum_required(VERSION 3.15)

# Тесты на C++ (Catch2). Выключить для сборки на стенд: -DFMS_BUILD_TESTS=OFF.
# Должно стоять до project(): vcpkg читает список features в момент project()
option(FMS_BUILD_TESTS "Build C++ unit tests (Catch2)" ON)
if(FMS_BUILD_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()

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

# ctest: должно стоять до add_subdirectory, иначе тесты из подпапок не зарегистрируются
if(FMS_BUILD_TESTS)
    enable_testing()
endif()

#add_subdirectory(shared/mqtt_client)
add_subdirectory(vda5050)
add_subdirectory(proto)
add_subdirectory(shared)
add_subdirectory(fleet_control)
add_subdirectory(fleet_dispatch)
add_subdirectory(fleet_database)
add_subdirectory(robot_simulation)

#add_library(mqtt_client STATIC
#        shared/src/mqtt_client.cpp
#        shared/include/mqtt_client.hpp
#)
#
#target_include_directories(mqtt_client PUBLIC
#        ${CMAKE_CURRENT_SOURCE_DIR}/fleet_dispatch/include
#)


#target_link_libraries(mqtt_client PRIVATE Boost::headers)