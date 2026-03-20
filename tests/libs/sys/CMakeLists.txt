# =============================================================================
# tests/sys/CMakeLists.txt — sys category tests (all gtest-based)
# =============================================================================

mk_add_gtest(signal_logger_test          log/signal_logger_test.cpp              mk_sys)
mk_add_gtest(async_logger_test           log/async_logger_test.cpp               mk_sys)
mk_add_gtest(global_new_delete_test      memory/global_new_delete_test.cpp       mk_sys)
mk_add_gtest(lock_free_stack_test        memory/lock_free_stack_test.cpp         mk_sys)
mk_add_gtest(intrusive_lock_free_stack_test memory/intrusive_lock_free_stack_test.cpp mk_sys)
mk_add_gtest(single_thread_stack_test    memory/single_thread_stack_test.cpp     mk_sys)
mk_add_gtest(mmap_region_test            memory/mmap_region_test.cpp             mk_sys)
mk_add_gtest(object_pool_test            memory/object_pool_test.cpp             mk_sys)
mk_add_gtest(aligned_new_allocator_test  memory/aligned_new_allocator_test.cpp   mk_sys)
mk_add_gtest(aligned_array_deleter_test memory/aligned_array_deleter_test.cpp  mk_sys)
mk_add_gtest(spsc_queue_test             memory/spsc_queue_test.cpp              mk_sys)
mk_add_gtest(fixed_spsc_queue_test       memory/fixed_spsc_queue_test.cpp        mk_sys)
mk_add_gtest(affinity_test              thread/affinity_test.cpp                mk_sys)
mk_add_gtest(nano_clock_test            nano_clock_test.cpp                     mk_sys)
mk_add_gtest(endian_test               endian_test.cpp                         mk_sys)
mk_add_gtest(arena_allocator_test       memory/arena_allocator_test.cpp         mk_sys)
mk_add_gtest(mmap_utils_test           memory/mmap_utils_test.cpp              mk_sys)
mk_add_gtest(bit_utils_test            bit_utils_test.cpp                      mk_sys)
mk_add_gtest(xorshift64_test           xorshift64_test.cpp                     mk_sys)
mk_add_gtest(string_pack_test          string_pack_test.cpp                    mk_sys)

# mmap_utils_test validates the internal/expert API as well.
target_include_directories(mmap_utils_test PRIVATE
    ${PROJECT_SOURCE_DIR}/libs/sys/private
)
target_compile_definitions(mmap_utils_test PRIVATE MK_SYS_INTERNAL_API)

# Internal-header tests.
target_include_directories(aligned_new_allocator_test PRIVATE
    ${PROJECT_SOURCE_DIR}/libs/sys/private
)
target_compile_definitions(aligned_new_allocator_test PRIVATE MK_SYS_INTERNAL_API)

target_include_directories(string_pack_test PRIVATE
    ${PROJECT_SOURCE_DIR}/libs/sys/private
)
target_compile_definitions(string_pack_test PRIVATE MK_SYS_INTERNAL_API)
