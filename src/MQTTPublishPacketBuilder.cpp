#include "MQTTPublishPacketBuilder.hpp"


namespace nioev::mqtt {

std::atomic<uint16_t> MQTTPublishPacketBuilder::gPacketIdCounter{ 0 };

MQTTPublishPacketBuilder::MQTTPublishPacketBuilder(const std::string& topic, const std::vector<uint8_t>& payload, Retained retained)
: mTopic(topic), mPayload(payload), mRetained(retained) {
    mPackets.fill({});
}
SharedBuffer& MQTTPublishPacketBuilder::getPacket(QoS qos) {
    auto qosInt = static_cast<int>(qos);
    auto& packetSlot = mPackets.at(qosInt);
    if(packetSlot.has_value()) {
        return packetSlot.value();
    }
    BinaryEncoder encoder;
    uint8_t firstByte = static_cast<uint8_t>(qos) << 1;
    if(mRetained == Retained::Yes) {
        firstByte |= 1;
    }
    firstByte |= static_cast<uint8_t>(MQTTMessageType::PUBLISH) << 4;
    encoder.encodeByte(firstByte);
    encoder.encodeString(mTopic);
    if(qos != QoS::QoS0) {
        // we use a global id here to increase performance - this could cause trouble when you have many clients, so it's not *that* spec compliant
        // we should have an option for using per-client id counters
        // TODO make this behaviour configurable in a config file
        uint16_t id = 0;
        while(!id) {
            // packet identifier must be non-zero
            id = gPacketIdCounter.fetch_add(1);
        }
        encoder.encodePacketId(id);
    }
    encoder.encodeBytes(mPayload);
    encoder.insertPacketLength();

    packetSlot = encoder.moveData();
    return packetSlot.value();
}

}