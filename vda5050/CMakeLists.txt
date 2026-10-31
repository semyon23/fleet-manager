#make_minimum_required(VERSION 3.12)
project(vda5050_lib VERSION 1.0.0 LANGUAGES CXX)

add_library(vda5050 INTERFACE)


target_include_directories(vda5050 INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_compile_features(vda5050 INTERFACE cxx_std_17)

