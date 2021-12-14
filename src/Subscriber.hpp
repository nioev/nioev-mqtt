#pragma once
#include "Enums.hpp"
#include <string>
#include <vector>

namespace nioev {

class Subscriber {
public:
    virtual void publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained) = 0;
};

}