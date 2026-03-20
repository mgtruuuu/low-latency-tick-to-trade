# =============================================================================
# tests/ds/CMakeLists.txt — ds category tests (all gtest-based)
# =============================================================================

mk_add_gtest(hash_utils_test       hash_utils_test.cpp       mk_ds)
mk_add_gtest(fixed_hash_map_test  fixed_hash_map_test.cpp  mk_ds)
mk_add_gtest(fixed_ring_buffer_test  fixed_ring_buffer_test.cpp  mk_ds)
mk_add_gtest(intrusive_list_test  intrusive_list_test.cpp  mk_ds)
mk_add_gtest(fixed_index_free_stack_test  fixed_index_free_stack_test.cpp  mk_ds)
mk_add_gtest(index_free_stack_test  index_free_stack_test.cpp  mk_ds)
mk_add_gtest(ring_buffer_test        ring_buffer_test.cpp        mk_ds)
mk_add_gtest(hash_map_test           hash_map_test.cpp           mk_ds)
mk_add_gtest(fixed_timing_wheel_test  fixed_timing_wheel_test.cpp  mk_ds)
mk_add_gtest(timing_wheel_test        timing_wheel_test.cpp        mk_ds)
mk_add_gtest(fixed_latency_histogram_test  fixed_latency_histogram_test.cpp  mk_ds)
mk_add_gtest(fixed_latency_recorder_test  fixed_latency_recorder_test.cpp  mk_ds)

mk_add_gtest(absl_flat_hash_map_test  absl_flat_hash_map_test.cpp  mk_ds)
target_link_libraries(absl_flat_hash_map_test PRIVATE
    absl::flat_hash_map
    absl::hash
)
