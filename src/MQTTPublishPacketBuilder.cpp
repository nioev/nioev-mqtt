#include "MQTTPublishPacketBuilder.hpp"


namespace nioev::mqtt {

MQTTPublishPacketBuilder::MQTTPublishPacketBuilder(const std::string& topic, PayloadType payload, Retained retained, const PropertyList& properties)
: mTopic(topic), mPayload(payload), mRetained(retained), mProperties(properties) {

}
EncodedPacket MQTTPublishPacketBuilder::getPacket(QoS qos, uint16_t packetId, MQTTVersion version) {
    auto qosInt = static_cast<int>(qos);

    uint8_t firstByte = static_cast<uint8_t>(qos) << 1;
    if(mRetained == Retained::Yes) {
        firstByte |= 1;
    }
    firstByte |= static_cast<uint8_t>(MQTTMessageType::PUBLISH) << 4;

    if(mPacketPostlude.count(version) && mPacketMiddle) {
        if(qos == QoS::QoS0) {
            return EncodedPacket::fromComponents(firstByte, mPacketMiddle.value(), mPacketPostlude.at(version));
        } else {
            return EncodedPacket::fromComponents(firstByte, mPacketMiddle.value(), packetId, mPacketPostlude.at(version));
        }
    }

    BinaryEncoder middleEncoder;
    middleEncoder.encodeString(mTopic);
    mPacketMiddle = middleEncoder.moveData();

    BinaryEncoder endEncoder;
    if(version == MQTTVersion::V5) {
        endEncoder.encodePropertyList(mProperties);
    }
    endEncoder.encodeBytes(mPayload.data(), mPayload.size());

    mPacketPostlude.emplace(version, endEncoder.moveData());

    if(qos == QoS::QoS0) {
        return EncodedPacket::fromComponents(firstByte, mPacketMiddle.value(), mPacketPostlude.at(version));
    } else {
        return EncodedPacket::fromComponents(firstByte, mPacketMiddle.value(), packetId, mPacketPostlude.at(version));
    }
}

size_t EncodedPacket::constructIOVecs(size_t offset, iovec* iovecs) {
    size_t ret = 0;
    switch(mType) {
    case Type::SingleByteWithLen:
        assert(offset < mPreludeLength);
        iovecs[0].iov_len = mPreludeLength - offset;
        iovecs[0].iov_base = &mPrelude.firstByte + offset;
        return 1;
    case Type::SingleByteWithLenAndMiddle:
        if(offset < mPreludeLength) {
            iovecs[0].iov_len = mPreludeLength - offset;
            iovecs[0].iov_base = &mPrelude.firstByte + offset;
            iovecs[1].iov_len = mMiddle.size();
            iovecs[1].iov_base = mMiddle.data();
        } else {
            iovecs[1].iov_len = mMiddle.size() - (offset - mPreludeLength);
            iovecs[1].iov_base = mMiddle.data() + (offset - mPreludeLength);
        }
        return 2;
    case Type::SingleByteWithLenAndMiddleAndEnd:
        if(offset < mPreludeLength) {
            iovecs[0].iov_len = mPreludeLength - offset;
            iovecs[0].iov_base = &mPrelude.firstByte + offset;
            iovecs[1].iov_len = mMiddle.size();
            iovecs[1].iov_base = mMiddle.data();
            iovecs[2].iov_len = mEnd.size();
            iovecs[2].iov_base = mEnd.data();
        } else if(offset < mPreludeLength + mMiddle.size()) {
            iovecs[1].iov_len = mMiddle.size() - (offset - mPreludeLength);
            iovecs[1].iov_base = mMiddle.data() + (offset - mPreludeLength);
            iovecs[2].iov_len = mEnd.size();
            iovecs[2].iov_base = mEnd.data();
        } else {
            iovecs[2].iov_len = mEnd.size() - (offset - mPreludeLength - mMiddle.size());
            iovecs[2].iov_base = mEnd.data() + (offset - mPreludeLength - mMiddle.size());
        }
        return 3;
    case Type::Full:
        if(offset < mPreludeLength) {
            iovecs[0].iov_len = mPreludeLength - offset;
            iovecs[0].iov_base = &mPrelude.firstByte + offset;
            iovecs[1].iov_len = mMiddle.size();
            iovecs[1].iov_base = mMiddle.data();
            iovecs[2].iov_len = sizeof(uint16_t);
            iovecs[2].iov_base = &mPacketId.value();
            iovecs[3].iov_len = mEnd.size();
            iovecs[3].iov_base = mEnd.data();
        } else if(offset < mPreludeLength + mMiddle.size()) {
            iovecs[1].iov_len = mMiddle.size() - (offset - mPreludeLength);
            iovecs[1].iov_base = mMiddle.data() + (offset - mPreludeLength);
            iovecs[2].iov_len = sizeof(uint16_t);
            iovecs[2].iov_base = &mPacketId.value();
            iovecs[3].iov_len = mEnd.size();
            iovecs[3].iov_base = mEnd.data();
        } else if(offset < mPreludeLength + mMiddle.size() + sizeof(uint16_t)) {
            iovecs[2].iov_len = sizeof(uint16_t) - (offset - mPreludeLength - mMiddle.size());
            iovecs[2].iov_base = &mPacketId.value() + (offset - mPreludeLength - mMiddle.size());
            iovecs[3].iov_len = mEnd.size();
            iovecs[3].iov_base = mEnd.data();
        } else {
            iovecs[3].iov_len = mEnd.size() - (offset - mPreludeLength - mMiddle.size() - sizeof(uint16_t));
            iovecs[3].iov_base = mEnd.data() + (offset - mPreludeLength - mMiddle.size() - sizeof(uint16_t));
        }
        return 4;
    case Type::Invalid:
        assert(0);
        break;
    }
    assert(0);
    return 0;
}
}