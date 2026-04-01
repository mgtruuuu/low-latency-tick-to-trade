# -- exchange_engine (matching engine + shm queues) --
add_executable(exchange_engine exchange_engine_main.cpp)
target_link_libraries(exchange_engine PRIVATE
    mk_sys          # MmapRegion for shared memory
    mk_algo         # MatchingEngine
    mk_app_shared   # Protocol definitions
    absl::flags
    absl::flags_parse
    rt              # shm_open, shm_unlink
)
target_compile_definitions(exchange_engine PRIVATE _GNU_SOURCE)
if(MK_CLANG_TIDY_COMMAND)
    set_target_properties(exchange_engine PROPERTIES CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
endif()

# -- exchange_gateway (TCP server + shm IPC) --
add_executable(exchange_gateway exchange_gateway_main.cpp)
target_link_libraries(exchange_gateway PRIVATE
    mk_sys          # MmapRegion for shared memory
    mk_net          # TcpSocket, TcpServer, codecs
    mk_app_shared   # Protocol definitions
    absl::flags
    absl::flags_parse
    rt              # shm_open
)
target_compile_definitions(exchange_gateway PRIVATE _GNU_SOURCE)
if(MK_CLANG_TIDY_COMMAND)
    set_target_properties(exchange_gateway PROPERTIES CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
endif()

# -- exchange_md_publisher (shm read + UDP multicast) --
add_executable(exchange_md_publisher exchange_md_main.cpp)
target_link_libraries(exchange_md_publisher PRIVATE
    mk_sys          # MmapRegion for shared memory
    mk_net          # UdpSocket
    mk_app_shared   # Protocol definitions
    absl::flags
    absl::flags_parse
    rt              # shm_open
)
target_compile_definitions(exchange_md_publisher PRIVATE _GNU_SOURCE)
if(MK_CLANG_TIDY_COMMAND)
    set_target_properties(exchange_md_publisher PROPERTIES CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
endif()
