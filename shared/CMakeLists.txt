find_package(Boost REQUIRED COMPONENTS headers)
add_library(mqtt_client STATIC
        include/mqtt_client.cpp
        src/mqtt_client.h
)

target_include_directories(mqtt_client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

