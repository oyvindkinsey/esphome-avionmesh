#include "device_db.h"
#include <nvs.h>
#include <cassert>
#include <iostream>

int main() {
    avionmesh::DeviceDB home;
    home.set_passphrase("home-key-must-survive");
    home.add_device(500, 90, "Home device");
    const auto home_nvs = fake_nvs::data.at("avionmesh");
    auto synthetic = avionmesh::DeviceDB::cloud_scale_test_db();
    fake_nvs::opened.clear();
    assert(synthetic.replace_inventory_metadata({{32768, 90, "Synthetic", {}}}, {}));
    auto populated = avionmesh::DeviceDB::cloud_scale_test_db();
    populated.load();
    assert(populated.find_device(32768));
    assert(!populated.find_device(500));
    // Candidate copies must keep the test namespace even across a failed save.
    fake_nvs::fail_commit = true;
    assert(!populated.replace_inventory_metadata({{32769, 90, "Rejected", {}}}, {}));
    assert(populated.find_device(32768));
    assert(!populated.find_device(32769));
    fake_nvs::fail_commit = false;
    populated.clear();
    for (const auto &name : fake_nvs::opened)
        assert(name == "avion_scale");
    assert(fake_nvs::data.at("avionmesh") == home_nvs);
    assert(home.find_device(500));
    assert(home.passphrase() == "home-key-must-survive");
    std::cout << "synthetic NVS namespace/candidate/load/clear/failure isolation passed\n";
}
