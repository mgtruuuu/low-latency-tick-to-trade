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

add_executable(order_gateway_test order_gateway_test.cpp)
target_link_libraries(order_gateway_test PRIVATE
    mk_app_shared
    mk_net
    GTest::gtest_main
)
gtest_discover_tests(order_gateway_test)
if(MK_CLANG_TIDY_COMMAND)
    set_target_properties(order_gateway_test PROPERTIES
        CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
endif()

add_executable(replay_order_gateway_test replay_order_gateway_test.cpp)
target_link_libraries(replay_order_gateway_test PRIVATE
    mk_app_shared
    mk_net
    GTest::gtest_main
)
gtest_discover_tests(replay_order_gateway_test)
if(MK_CLANG_TIDY_COMMAND)
    set_target_properties(replay_order_gateway_test PROPERTIES
        CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
endif()
