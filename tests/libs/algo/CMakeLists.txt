# =============================================================================
# tests/algo/CMakeLists.txt — algo category tests (all gtest-based)
# =============================================================================

mk_add_gtest(order_book_test       order_book_test.cpp       mk_algo)
mk_add_gtest(matching_engine_test  matching_engine_test.cpp  mk_algo)
