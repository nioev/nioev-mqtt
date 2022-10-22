#pragma once
#include "nioev/lib/Enums.hpp"
#include "nioev/lib/Util.hpp"
#include <string>
#include <vector>
#include <memory>

#include "Forward.hpp"
#include "TaskQueueRefCount.hpp"

namespace nioev::mqtt {
using namespace nioev::lib;

class Subscriber : public TaskQueueRefCount {
public:
    virtual void publish(const std::string& topic, PayloadType payload, QoS qos, Retained retained, const PropertyList& properties, MQTTPublishPacketBuilder& packetBuilder) = 0;
    virtual ~Subscriber() = default;

    virtual bool isDeleted() const {
        return false;
    }

    virtual const char* getType() const = 0;
};

}