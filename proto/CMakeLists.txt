
file(GLOB PROTO_FILES "*.proto")

if(TARGET protobuf::protoc)
    get_target_property(PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_RELEASE)
    if(NOT PROTOC_EXECUTABLE)
        get_target_property(PROTOC_EXECUTABLE protobuf::protoc IMPORTED_LOCATION_DEBUG)
    endif()

    if(NOT PROTOC_EXECUTABLE)
        set(PROTOC_EXECUTABLE $<TARGET_FILE:protobuf::protoc>)
    endif()
endif()


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
