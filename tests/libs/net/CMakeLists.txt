# =============================================================================
# tests/net/CMakeLists.txt — net category tests (all gtest-based)
# =============================================================================

mk_add_gtest(socket_base_test     socket_base_test.cpp     mk_net)
mk_add_gtest(scoped_fd_test       scoped_fd_test.cpp       mk_net)
mk_add_gtest(tcp_socket_test      tcp_socket_test.cpp      mk_net)
mk_add_gtest(epoll_wrapper_test   epoll_wrapper_test.cpp   mk_net)
mk_add_gtest(udp_socket_test      udp_socket_test.cpp      mk_net)
mk_add_gtest(wire_writer_test    wire_writer_test.cpp     mk_net)
mk_add_gtest(wire_reader_test    wire_reader_test.cpp     mk_net)
mk_add_gtest(message_codec_test  message_codec_test.cpp   mk_net)
mk_add_gtest(length_prefix_codec_test length_prefix_codec_test.cpp mk_net)
mk_add_gtest(tcp_server_test    tcp_server_test.cpp    mk_net)
mk_add_gtest(packed_codec_test  packed_codec_test.cpp  mk_net)
