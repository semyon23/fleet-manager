#make_minimum_required(VERSION 3.12)
project(vda5050_lib VERSION 1.0.0 LANGUAGES CXX)

add_library(vda5050 INTERFACE)

# Путь указывает на саму папку vda5050, файлы .hpp будут доступны как <common.hpp>
target_include_directories(vda5050 INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# Указываем, что заголовкам требуется минимум C++17 (совместимо с вашим C++23)
target_compile_features(vda5050 INTERFACE cxx_std_17)

