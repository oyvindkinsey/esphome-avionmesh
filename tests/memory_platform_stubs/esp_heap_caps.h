#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>

constexpr uint32_t MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2, MALLOC_CAP_SPIRAM = 4;
namespace memory_test {
struct Pool {
    size_t total, free, minimum, largest;
    unsigned total_reads{0}, metric_reads{0};
};
inline Pool internal{100000, 40000, 23000, 12000};
inline Pool psram{8000000, 7000000, 6000000, 5000000};
inline Pool &pool(uint32_t caps) {
    if (caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT))
        return internal;
    if (caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))
        return psram;
    std::abort();  // Wrong capability selection is a test failure.
}
}  // namespace memory_test
inline size_t heap_caps_get_total_size(uint32_t caps) {
    auto &pool = memory_test::pool(caps);
    ++pool.total_reads;
    return pool.total;
}
inline size_t heap_caps_get_free_size(uint32_t caps) {
    auto &pool = memory_test::pool(caps);
    ++pool.metric_reads;
    return pool.free;
}
inline size_t heap_caps_get_minimum_free_size(uint32_t caps) {
    auto &pool = memory_test::pool(caps);
    ++pool.metric_reads;
    return pool.minimum;
}
inline size_t heap_caps_get_largest_free_block(uint32_t caps) {
    auto &pool = memory_test::pool(caps);
    ++pool.metric_reads;
    return pool.largest;
}
