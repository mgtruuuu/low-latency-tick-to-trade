add_executable(tick_to_trade main.cpp)
target_link_libraries(tick_to_trade PRIVATE
    mk_net          # TcpSocket, UdpSocket, EpollWrapper, codecs
    mk_algo         # OrderBook types (Price, Side, Qty)
    mk_app_shared   # Shared protocol definitions
    absl::flags
    absl::flags_parse
)
target_compile_definitions(tick_to_trade PRIVATE _GNU_SOURCE)

# Per-stage latency instrumentation (feed parse, strategy, order send).
# Always-on: wire-to-wire tick-to-trade measurement (2 RDTSC calls).
# PROFILE_STAGES adds per-stage breakdown (4 extra RDTSC calls + histogram records,
# ~50ns overhead per tick). Enable for bottleneck analysis, disable for production.
option(PROFILE_STAGES "Enable per-stage latency instrumentation" ON)
if(PROFILE_STAGES)
    target_compile_definitions(tick_to_trade PRIVATE PROFILE_STAGES)
endif()
if(MK_CLANG_TIDY_COMMAND)
    set_target_properties(tick_to_trade PROPERTIES CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
endif()
