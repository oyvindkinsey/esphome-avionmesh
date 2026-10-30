#pragma once
#include <string>

namespace esphome {

class Application {
 public:
    std::string get_name() const { return "test"; }
    void feed_wdt() {}
};

extern Application App;

}  // namespace esphome
