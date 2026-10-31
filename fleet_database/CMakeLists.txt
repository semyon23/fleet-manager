find_package(libpqxx CONFIG REQUIRED)
find_package(asio-grpc CONFIG REQUIRED)

add_executable(fleet_database
        fdb_main.cpp
        src/Database.cpp
        src/DatabaseService.cpp
        src/RobotTable.cpp
        src/ApiJson.cpp
        include/ApiJson.hpp
        src/RestApi.cpp
        include/RestApi.hpp
)

target_include_directories(fleet_database PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(fleet_database PRIVATE
        http_server
        grpc_server            # GrpcServer из shared, путь shared/include
        proto_lib              # сгенерированные классы и путь к *.pb.h
        asio-grpc::asio-grpc   # agrpc + макрос бэкенда Boost.Asio
        Boost::headers
        libpqxx::pqxx
        spdlog::spdlog
)

target_compile_definitions(fleet_database PRIVATE FMS_MIGRATIONS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/migrations")

#set(CMAKE_CXX_STANDARD 23)
#set(CMAKE_CXX_STANDARD_REQUIRED ON)
#
#find_package(Boost REQUIRED COMPONENTS headers)
#find_package(asio-grpc CONFIG REQUIRED)
#find_package(nlohmann_json CONFIG REQUIRED)
#find_package(libpqxx CONFIG REQUIRED)
#
#add_executable(fleet_database fdb_main.cpp
#        include/DatabaseService.cpp
#        include/Database.cpp
#)
#
#target_link_libraries(fleet_dispatch
#        PRIVATE
#        vda5050
#        spdlog::spdlog
#        libpqxx::pqxx
#        asio-grpc::asio-grpc
#        nlohmann_json::nlohmann_json
#        proto_lib
#        gRPC::grpc++_reflection
#)
#
#target_include_directories(fleet_database PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
#target_link_libraries(fleet_database PRIVATE Boost::headers)
#target_link_libraries(fleet_database PRIVATE
#        libpqxx::pqxx
#        spdlog::spdlog
#        Boost::headers
#)