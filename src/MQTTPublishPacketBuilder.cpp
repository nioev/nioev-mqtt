#include "MQTTPublishPacketBuilder.hpp"

namespace nioev {

std::atomic<uint16_t> MQTTPublishPacketBuilder::gPacketIdCounter{ 0 };

MQTTPublishPacketBuilder::MQTTPublishPacketBuilder(const std::string& topic, const std::vector<uint8_t>& payload, Retained retained)
: mTopic(topic), mPayload(payload), mRetained(retained) {
    mPackets.fill({});
}
util::SharedBuffer& MQTTPublishPacketBuilder::getPacket(QoS qos) {
    qos = QoS::QoS0;
    auto qosInt = static_cast<int>(qos);
    auto& packetSlot = mPackets.at(qosInt);
    if(packetSlot.has_value()) {
        return packetSlot.value();
    }
    util::BinaryEncoder encoder;
    if(qos == QoS::QoS2) // FIXME QoS 2 support
        qos = QoS::QoS1;
    uint8_t firstByte = static_cast<uint8_t>(qos) << 1;
    // TODO dup
    if(mRetained == Retained::Yes) {
        firstByte |= 1;
    }
    firstByte |= static_cast<uint8_t>(MQTTMessageType::PUBLISH) << 4;
    encoder.encodeByte(firstByte);
    encoder.encodeString(mTopic);
    if(qos != QoS::QoS0) {
        encoder.encode2Bytes(gPacketIdCounter.fetch_add(1));
    }
    encoder.encodeBytes(mPayload);
    encoder.insertPacketLength();

    packetSlot = encoder.moveData();
    return packetSlot.value();
}

}