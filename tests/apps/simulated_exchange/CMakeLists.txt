# =============================================================================
# tests/apps/simulated_exchange/CMakeLists.txt — app-layer tests (gtest-based)
# =============================================================================
# Tests for apps/ code. These need both mk_app_shared (protocol headers)
# and mk_net (socket concept, endian utilities), so we link manually
# instead of using mk_add_gtest (which takes a single library).

add_executable(market_data_publisher_test market_data_publisher_test.cpp)
target_link_libraries(market_data_publisher_test PRIVATE
    mk_app_shared
    mk_net
    GTest::gtest_main
)
gtest_discover_tests(market_data_publisher_test)
if(MK_CLANG_TIDY_COMMAND)
    set_target_properties(market_data_publisher_test PROPERTIES
        CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
endif()

add_executable(exchange_core_test exchange_core_test.cpp)
target_link_libraries(exchange_core_test PRIVATE
    mk_app_shared
    mk_net
    GTest::gtest_main
)
gtest_discover_tests(exchange_core_test)
if(MK_CLANG_TIDY_COMMAND)
    set_target_properties(exchange_core_test PROPERTIES
        CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
endif()

add_executable(message_codec_framer_test message_codec_framer_test.cpp)
target_link_libraries(message_codec_framer_test PRIVATE
    mk_app_shared
    mk_net
    GTest::gtest_main
)
gtest_discover_tests(message_codec_framer_test)
if(MK_CLANG_TIDY_COMMAND)
    set_target_properties(message_codec_framer_test PROPERTIES
        CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
endif()

add_executable(shared_exchange_queue_test shared_exchange_queue_test.cpp)
target_link_libraries(shared_exchange_queue_test PRIVATE
    mk_app_shared
    mk_sys
    mk_net
    GTest::gtest_main
)
target_link_libraries(shared_exchange_queue_test PRIVATE rt)  # shm_open/shm_unlink
gtest_discover_tests(shared_exchange_queue_test)
if(MK_CLANG_TIDY_COMMAND)
    set_target_properties(shared_exchange_queue_test PROPERTIES
        CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
endif()

