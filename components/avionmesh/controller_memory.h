#pragma once

#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>

#ifdef USE_ESP32
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace avionmesh {

struct ControllerHeapSample {
    bool available{false};
    const char *unavailable_reason{"unsupported_platform"};
    size_t total_bytes{0}, free_bytes{0}, minimum_free_bytes{0}, largest_free_block_bytes{0};
};

struct ControllerMemorySample {
    uint32_t sampled_at_ms{0};
    const char *platform{"unsupported"};
    ControllerHeapSample internal, psram;
    bool task_available{false}, stack_minimum_available{false};
    const char *task_name{nullptr};
    size_t stack_minimum_free_bytes{0};
};

#ifdef USE_ESP32
inline ControllerHeapSample sample_controller_heap(uint32_t capabilities) {
    ControllerHeapSample sample;
    sample.total_bytes = heap_caps_get_total_size(capabilities);
    if (!sample.total_bytes) {
        sample.unavailable_reason = "no_matching_heap";
        return sample;
    }
    sample.available = true;
    sample.unavailable_reason = nullptr;
    sample.free_bytes = heap_caps_get_free_size(capabilities);
    sample.minimum_free_bytes = heap_caps_get_minimum_free_size(capabilities);
    sample.largest_free_block_bytes = heap_caps_get_largest_free_block(capabilities);
    return sample;
}
#endif

// Read-only, point-in-time sampling. Do this before allocating the status JSON.
// Never start/reset ESP-IDF's local-minimum heap monitor: minima retain their
// allocator lifetime scope (sum of per-region low-water marks, not a phase min).
inline ControllerMemorySample sample_controller_memory(uint32_t now_ms) {
    ControllerMemorySample sample;
    sample.sampled_at_ms = now_ms;
#ifdef USE_ESP32
    sample.platform = "esp_idf";
    sample.internal = sample_controller_heap(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    sample.psram = sample_controller_heap(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    sample.task_available = true;
    sample.task_name = pcTaskGetName(nullptr);
#if defined(INCLUDE_uxTaskGetStackHighWaterMark) && INCLUDE_uxTaskGetStackHighWaterMark
    sample.stack_minimum_available = true;
    // ESP-IDF returns bytes here, unlike upstream FreeRTOS's word-based API.
    sample.stack_minimum_free_bytes = uxTaskGetStackHighWaterMark(nullptr);
#endif
#endif
    return sample;
}

inline void write_controller_heap(JsonObject output, const ControllerHeapSample &sample,
                                  const char *capabilities) {
    output["available"] = sample.available;
    output["capabilities"] = capabilities;
    output["unavailable_reason"] = sample.available ? nullptr : sample.unavailable_reason;
    if (sample.available) {
        output["total_bytes"] = sample.total_bytes;
        output["free_bytes"] = sample.free_bytes;
        output["minimum_free_bytes"] = sample.minimum_free_bytes;
        output["largest_free_block_bytes"] = sample.largest_free_block_bytes;
    } else {
        output["total_bytes"] = nullptr;
        output["free_bytes"] = nullptr;
        output["minimum_free_bytes"] = nullptr;
        output["largest_free_block_bytes"] = nullptr;
    }
}

inline void write_controller_memory(JsonObject output, const ControllerMemorySample &sample) {
    output["schema_version"] = 1;
    output["units"] = "bytes";
    output["platform"] = sample.platform;
    output["sampled_at_ms"] = sample.sampled_at_ms;
    output["sample_scope"] = "before_status_response_serialization";
    output["minimum_scope"] = "boot_lifetime_per_region_sum";
    write_controller_heap(output["internal"].to<JsonObject>(), sample.internal, "internal_8bit");
    write_controller_heap(output["psram"].to<JsonObject>(), sample.psram, "spiram_8bit");
    auto task = output["task"].to<JsonObject>();
    task["available"] = sample.task_available;
    task["context"] = "controller_status_calling_task";
    task["name"] = sample.task_name;
    task["stack_minimum_available"] = sample.stack_minimum_available;
    task["stack_minimum_scope"] = "task_lifetime";
    if (sample.stack_minimum_available)
        task["stack_minimum_free_bytes"] = sample.stack_minimum_free_bytes;
    else
        task["stack_minimum_free_bytes"] = nullptr;
}

}  // namespace avionmesh
