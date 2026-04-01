# =============================================================================
# tests/apps/tick_to_trade/CMakeLists.txt — pipeline component unit tests
# =============================================================================

add_executable(feed_handler_test feed_handler_test.cpp)
target_link_libraries(feed_handler_test PRIVATE
    mk_app_shared
    mk_net
    GTest::gtest_main
)
gtest_discover_tests(feed_handler_test)

add_executable(strategy_test strategy_test.cpp)
target_link_libraries(strategy_test PRIVATE
    mk_app_shared
    mk_algo
    GTest::gtest_main
)
gtest_discover_tests(strategy_test)

add_executable(order_manager_test order_manager_test.cpp)
target_link_libraries(order_manager_test PRIVATE
    mk_app_shared
    mk_ds
    mk_net
    GTest::gtest_main
)
gtest_discover_tests(order_manager_test)

add_executable(order_response_handler_test order_response_handler_test.cpp)
target_link_libraries(order_response_handler_test PRIVATE
    mk_app_shared
    mk_ds
    mk_net
    GTest::gtest_main
)
gtest_discover_tests(order_response_handler_test)

add_executable(order_send_handler_test order_send_handler_test.cpp)
target_link_libraries(order_send_handler_test PRIVATE
    mk_app_shared
    mk_ds
    mk_net
    GTest::gtest_main
)
gtest_discover_tests(order_send_handler_test)

add_executable(pipeline_log_formatter_test pipeline_log_formatter_test.cpp)
target_link_libraries(pipeline_log_formatter_test PRIVATE
    mk_app_shared
    mk_sys
    GTest::gtest_main
)
gtest_discover_tests(pipeline_log_formatter_test)

if(MK_CLANG_TIDY_COMMAND)
    set_target_properties(feed_handler_test strategy_test order_manager_test
        order_response_handler_test order_send_handler_test
        pipeline_log_formatter_test
        PROPERTIES CXX_CLANG_TIDY "${MK_CLANG_TIDY_COMMAND}")
endif()
