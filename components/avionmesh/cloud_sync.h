#pragma once

#if __has_include("esphome/core/defines.h")
#include "esphome/core/defines.h"
#endif

#include "cloud_inventory.h"
#include <mutex>

namespace avionmesh {
class CloudSync {
 public:
    // Called on the HTTP task. Credentials remain in RAM for this job only.
    bool queue(std::string email, std::string password);
    std::string status_json();
    // Called on the main loop; returns true after a validated inventory merge.
    bool loop(DeviceDB &db);

 private:
    std::mutex mutex_;
    bool busy_{false}, queued_{false}, worker_done_{false};
    std::string state_{"idle"}, error_, email_, password_, expected_key_;
    CloudInventory inventory_;
#ifdef AVIONMESH_CLOUD_SCALE_TEST
    // Synthetic metadata only: never returned to the hub or written to home NVS.
#ifdef AVIONMESH_CLOUD_SCALE_PERSIST
    DeviceDB scale_db_{DeviceDB::cloud_scale_test_db()};
    bool scale_loaded_{false};
#else
    std::vector<DeviceEntry> scale_devices_;
    std::vector<GroupEntry> scale_groups_;
#endif
#endif
    unsigned added_devices_{0}, added_groups_{0};
    void progress(const char *state);
    void finish(const std::string &error);
    void worker();
    static void task_entry(void *arg);
};
}  // namespace avionmesh
