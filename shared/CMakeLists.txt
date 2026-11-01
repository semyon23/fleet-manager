add_library(mqtt_client STATIC
        src/mqtt_client.cpp
        include/mqtt_client.hpp
)
target_include_directories(mqtt_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(mqtt_client PUBLIC Boost::headers spdlog::spdlog)

add_library(grpc_server STATIC
        src/GrpcServer.cpp
        include/GrpcServer.hpp
)
target_include_directories(grpc_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(grpc_server PUBLIC gRPC::grpc++ gRPC::grpc++_reflection spdlog::spdlog)

add_library(http_server STATIC
        src/http_server.cpp
        include/http_server.hpp
)
target_include_directories(http_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(http_server PUBLIC Boost::headers spdlog::spdlog)

add_library(env_config INTERFACE)
target_include_directories(env_config INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(env_config INTERFACE Boost::headers)