# Поиск всех .proto файлов
file(GLOB PROTO_FILES "*.proto")

# 1. Надежный способ получить путь к protoc из таргета vcpkg
if(TARGET protobuf::protoc)
    get_target_property(PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_RELEASE)
    if(NOT PROTOC_EXECUTABLE)
        get_target_property(PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_DEBUG)
    endif()
    # Если свойства выше не сработали (зависит от версии CMake/vcpkg),
    # используем генераторное выражение (но аккуратно)
    if(NOT PROTOC_EXECUTABLE)
        set(PROTOC_EXECUTABLE $<TARGET_FILE:protobuf::protoc>)
    endif()
endif()

# 2. Получаем путь к gRPC плагину
set(GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)

set(GEN_PATH "${CMAKE_CURRENT_BINARY_DIR}")
set(ALL_GENERATED_SRCS "")

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(FIL_WE ${PROTO_FILE} NAME_WE)

    set(P_SRC "${GEN_PATH}/${FIL_WE}.pb.cc")
    set(P_HDR "${GEN_PATH}/${FIL_WE}.pb.h")
    set(G_SRC "${GEN_PATH}/${FIL_WE}.grpc.pb.cc")
    set(G_HDR "${GEN_PATH}/${FIL_WE}.grpc.pb.h")

    list(APPEND ALL_GENERATED_SRCS ${P_SRC} ${G_SRC})

    add_custom_command(
            OUTPUT "${P_SRC}" "${P_HDR}" "${G_SRC}" "${G_HDR}"
            COMMAND ${PROTOC_EXECUTABLE}
            ARGS --cpp_out=${GEN_PATH}
            --grpc_out=${GEN_PATH}
            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
            -I ${CMAKE_CURRENT_SOURCE_DIR}
            "${PROTO_FILE}"
            DEPENDS "${PROTO_FILE}"
            COMMENT "Compiling gRPC/Proto: ${PROTO_FILE}"
            VERBATIM
    )
endforeach()

add_library(proto_lib STATIC ${ALL_GENERATED_SRCS})
target_link_libraries(proto_lib PUBLIC protobuf::libprotobuf gRPC::grpc++)
target_include_directories(proto_lib PUBLIC ${GEN_PATH})

## Поиск всех .proto файлов в текущей папке
#file(GLOB PROTO_FILES "*.proto")
#
## Определяем пути к инструментам vcpkg (из find_package)
## Protobuf_PROTOC_EXECUTABLE обычно устанавливается find_package
#set(GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
#
## Путь, где будут лежать сгенерированные файлы
#set(GEN_PATH "${CMAKE_CURRENT_BINARY_DIR}")
#
## Список файлов, которые мы ожидаем получить (для сборки библиотеки)
#set(ALL_GEN_SRCS "")
#
#foreach(PROTO_FILE ${PROTO_FILES})
#    get_filename_component(FIL_WE ${PROTO_FILE} NAME_WE)
#
#    # Имена выходных файлов
#    set(P_SRCS "${GEN_PATH}/${FIL_WE}.pb.cc")
#    set(P_HDRS "${GEN_PATH}/${FIL_WE}.pb.h")
#    set(G_SRCS "${GEN_PATH}/${FIL_WE}.grpc.pb.cc")
#    set(G_HDRS "${GEN_PATH}/${FIL_WE}.grpc.pb.h")
#
#    list(APPEND ALL_GEN_SRCS ${P_SRCS} ${G_SRCS})
#
#    # Команда компиляции
#    add_custom_command(
#            OUTPUT ${P_SRCS} ${P_HDRS} ${G_SRCS} ${G_HDRS}
#            COMMAND Protobuf::protoc
#            ARGS --cpp_out=${GEN_PATH}
#            --grpc_out=${GEN_PATH}
#            --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN_EXECUTABLE}
#            -I ${CMAKE_CURRENT_SOURCE_DIR}
#            ${PROTO_FILE}
#            DEPENDS ${PROTO_FILE}
#            COMMENT "Compiling protocol buffer: ${PROTO_FILE}"
#            VERBATIM
#    )
#endforeach()
#
## Создаем библиотеку из сгенерированного кода
#add_library(proto_lib STATIC ${ALL_GEN_SRCS})
#
## Линкуем зависимости для самой библиотеки прототипов
#target_link_libraries(proto_lib
#        PUBLIC
#        protobuf::libprotobuf
#        gRPC::grpc++
#)
#
## Чтобы те, кто линкует proto_lib, автоматически видели путь к хедерам
#target_include_directories(proto_lib PUBLIC ${GEN_PATH})
