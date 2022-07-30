#pragma once
#include "nioev/lib/Enums.hpp"
#include <string>
#include <vector>
#include <memory>

#include "Forward.hpp"

namespace nioev {
using namespace nioev::lib;

class Subscriber : public std::enable_shared_from_this<Subscriber> {
public:
    virtual void publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained, MQTTPublishPacketBuilder& packetBuilder) = 0;
    virtual ~Subscriber() = default;

    auto makeShared() {
        return shared_from_this();
    }

    virtual const char* getType() const = 0;
};

}