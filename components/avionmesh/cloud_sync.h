#pragma once

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
    unsigned added_devices_{0}, added_groups_{0};
    void progress(const char *state);
    void finish(const std::string &error);
    void worker();
    static void task_entry(void *arg);
};
}  // namespace avionmesh
