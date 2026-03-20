add_executable(simulated_exchange main.cpp)
target_link_libraries(simulated_exchange PRIVATE
    mk_net          # TcpSocket, UdpSocket, EpollWrapper, codecs
    mk_algo         # MatchingEngine for order crossing
    mk_app_shared   # Shared protocol definitions
    absl::flags
    absl::flags_parse
)
target_compile_definitions(simulated_exchange PRIVATE _GNU_SOURCE)
if(MK_CLANG_TIDY_COMMAND)
    set_target_properties(simulated_exchange PROPERTIES CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
endif()
