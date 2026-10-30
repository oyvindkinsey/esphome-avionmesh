#pragma once
#include <cstddef>

// Compiled out entirely in production. No runtime endpoint or trust override.
#if defined(AVIONMESH_CLOUD_SCALE_PERSIST) && !defined(AVIONMESH_CLOUD_SCALE_TEST)
#error "Synthetic persistence requires the synthetic provider compile guard"
#endif
#ifdef AVIONMESH_CLOUD_SCALE_TEST
#ifndef AVIONMESH_CLOUD_SCALE_URL
#error "Synthetic scale build requires its HTTPS URL"
#endif
#ifndef AVIONMESH_CLOUD_SCALE_CA
#error "Synthetic scale build requires its pinned test CA"
#endif
#include <esp_heap_caps.h>
#include <mutex>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esphome/core/log.h"

namespace avionmesh {
inline void cloud_scale_memory(const char *phase, size_t body_bytes = 0,
                               TaskHandle_t task = nullptr, const char *task_name = nullptr) {
    // ESP-IDF FreeRTOS reports high water mark in BYTES, unlike upstream FreeRTOS.
    if (!task)
        task = xTaskGetCurrentTaskHandle();
    if (!task_name)
        task_name = pcTaskGetName(task);
    constexpr uint32_t internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    constexpr uint32_t psram = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI("avionmesh.scale",
             "CLOUD_SCALE {\"schema\":1,\"us\":%lld,\"phase\":\"%s\",\"task\":\"%s\","
             "\"body_bytes\":%u,\"internal_free\":%u,\"internal_min_global_boot\":%u,"
             "\"internal_largest\":%u,\"psram_free\":%u,\"psram_min_global_boot\":%u,"
             "\"psram_largest\":%u,\"stack_high_water_bytes\":%u}",
             static_cast<long long>(esp_timer_get_time()), phase, task_name,
             static_cast<unsigned>(body_bytes),
             static_cast<unsigned>(heap_caps_get_free_size(internal)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(internal)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(internal)),
             static_cast<unsigned>(heap_caps_get_free_size(psram)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(psram)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(psram)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(task)));
}
// Static callback state outlives timer deletion; the mutex protects the worker
// handle until the sampler has stopped, even if a callback was already running.
inline std::mutex cloud_scale_sampler_mutex;
inline TaskHandle_t cloud_scale_sampled_worker = nullptr;
class CloudScaleSampler {
 public:
    CloudScaleSampler() : worker_(xTaskGetCurrentTaskHandle()) {
        {
            std::lock_guard<std::mutex> lock(cloud_scale_sampler_mutex);
            cloud_scale_sampled_worker = worker_;
        }
        esp_timer_create_args_t args{};
        args.callback = [](void *arg) {
            std::lock_guard<std::mutex> lock(cloud_scale_sampler_mutex);
            if (cloud_scale_sampled_worker)
                cloud_scale_memory("periodic_worker", 0, cloud_scale_sampled_worker, "avion_cloud");
        };
        args.arg = nullptr;
        args.name = "cloud_scale";
        // ESP_TIMER_TASK callbacks are serialized. Stop/delete before worker exits.
        if (esp_timer_create(&args, &timer_) != ESP_OK ||
            esp_timer_start_periodic(timer_, 50000) != ESP_OK)
            cloud_scale_memory("sampler_unavailable");
    }
    ~CloudScaleSampler() {
        if (timer_)
            esp_timer_stop(timer_);
        {
            std::lock_guard<std::mutex> lock(cloud_scale_sampler_mutex);
            if (cloud_scale_sampled_worker == worker_)
                cloud_scale_sampled_worker = nullptr;
        }
        if (timer_)
            esp_timer_delete(timer_);
    }
 private:
    TaskHandle_t worker_;
    esp_timer_handle_t timer_{nullptr};
};
}  // namespace avionmesh
#else
namespace avionmesh {
inline void cloud_scale_memory(const char *, size_t = 0) {}
class CloudScaleSampler {};
}  // namespace avionmesh
#endif
