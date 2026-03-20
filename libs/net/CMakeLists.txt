# =============================================================================
# libs/net/CMakeLists.txt — Build definition for the mk_net library
# =============================================================================
#
# Header-only networking wrappers (no .cpp files).
# INTERFACE library: no compilation, only propagates include paths and deps.
#
# Components: ScopedFd, SocketBase, TcpSocket, UdpSocket, EpollWrapper
# Dependencies: mk_sys (for sys/log/signal_logger.hpp used by SocketBase, EpollWrapper)
#
# =============================================================================

add_library(mk_net INTERFACE)
target_include_directories(mk_net INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mk_net INTERFACE mk_sys)
