#include "controller_memory.h"
#include <cassert>
#include <string>

int main() {
    using namespace avionmesh;
    const auto sample = sample_controller_memory(123);
    assert(sample.internal.available && sample.psram.available);
    assert(sample.internal.free_bytes == 40000 && sample.internal.minimum_free_bytes == 23000);
    assert(sample.internal.largest_free_block_bytes == 12000);
    assert(sample.psram.free_bytes == 7000000 && sample.psram.minimum_free_bytes == 6000000);
    assert(sample.psram.largest_free_block_bytes == 5000000);
    assert(sample.task_available && std::string(sample.task_name) == "synthetic_calling_task");
    JsonDocument document;
    write_controller_memory(document.to<JsonObject>(), sample);
    assert(document["internal"]["capabilities"].as<std::string>() == "internal_8bit");
    assert(document["psram"]["capabilities"].as<std::string>() == "spiram_8bit");
#if INCLUDE_uxTaskGetStackHighWaterMark
    assert(memory_test::stack_reads == 1 && sample.stack_minimum_available);
    assert(document["task"]["stack_minimum_free_bytes"].as<size_t>() == 4097);
#else
    assert(memory_test::stack_reads == 0 && !sample.stack_minimum_available);
    assert(document["task"]["stack_minimum_free_bytes"].isNull());
#endif
    assert(memory_test::internal.total_reads == 1 && memory_test::internal.metric_reads == 3);
    assert(memory_test::psram.total_reads == 1 && memory_test::psram.metric_reads == 3);
    // A platform without PSRAM must not report fabricated measured zeros.
    memory_test::psram.total = 0;
    const auto no_psram = sample_controller_memory(124);
    document.clear();
    write_controller_memory(document.to<JsonObject>(), no_psram);
    assert(!document["psram"]["available"].as<bool>());
    assert(document["psram"]["unavailable_reason"].as<std::string>() == "no_matching_heap");
    assert(document["psram"]["free_bytes"].isNull());
    assert(memory_test::psram.metric_reads == 3);
    // Zero is a real measured value when the matching heap does exist.
    memory_test::internal.free = memory_test::internal.minimum = memory_test::internal.largest = 0;
    const auto exhausted = sample_controller_memory(125);
    document.clear();
    write_controller_memory(document.to<JsonObject>(), exhausted);
    assert(document["internal"]["available"].as<bool>());
    assert(document["internal"]["free_bytes"].is<size_t>());
    assert(document["internal"]["free_bytes"].as<size_t>() == 0);
}
