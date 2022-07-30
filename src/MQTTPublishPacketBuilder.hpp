#pragma once

#include "nioev/lib/Util.hpp"
#include <array>

namespace nioev {
using namespace nioev::lib;

class MQTTPublishPacketBuilder {
public:
    // A reference to topic & payload is captured, so be cautious about lifetimes!
    MQTTPublishPacketBuilder(const std::string& topic, const std::vector<uint8_t>& payload, Retained retained);
    lib::SharedBuffer& getPacket(QoS qos);
private:
    const std::string& mTopic;
    const std::vector<uint8_t>& mPayload;
    Retained mRetained;
    std::array<std::optional<SharedBuffer>, 3> mPackets;
    static std::atomic<uint16_t> gPacketIdCounter;
};

}