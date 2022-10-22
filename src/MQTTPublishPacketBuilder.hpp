#pragma once

#include "nioev/lib/Util.hpp"
#include "Subscriber.hpp"
#include <array>
#include <functional>
namespace nioev::mqtt {
using namespace nioev::lib;


class EncodedPacket {
    enum class Type {
        Invalid,
        SingleByteWithLen,
        SingleByteWithLenAndMiddle,
        SingleByteWithLenAndMiddleAndEnd,
        Full
    };
public:
    static EncodedPacket fromFirstByte(uint8_t firstByte) {
        EncodedPacket ret;
        ret.mType = Type::SingleByteWithLen;
        ret.mPrelude.firstByte = firstByte;
        memset(ret.mPrelude.varLength, 0, 4);
        ret.mPreludeLength = 2;
        return ret;
    }
    static EncodedPacket fromData(uint8_t firstByte, SharedBuffer&& buffer) {
        EncodedPacket ret;
        ret.mType = Type::SingleByteWithLenAndMiddle;
        ret.mMiddle = std::move(buffer);
        ret.mPrelude.firstByte = firstByte;
        auto varLength = encodeVarByteInt(buffer.size());
        memcpy(ret.mPrelude.varLength, varLength.value, varLength.valueLength);
        ret.mPreludeLength = varLength.valueLength + 1;
        return ret;
    }
    static EncodedPacket fromComponents(uint8_t firstByte, SharedBuffer middle, uint16_t packetId, SharedBuffer end) {
        EncodedPacket ret;
        ret.mType = Type::Full;
        auto varLength = encodeVarByteInt(end.size() + middle.size() + 2);
        ret.mMiddle = std::move(middle);
        ret.mPrelude.firstByte = firstByte;
        memcpy(ret.mPrelude.varLength, varLength.value, varLength.valueLength);
        ret.mPreludeLength = varLength.valueLength + 1;
        ret.mPacketId = htons(packetId);
        ret.mEnd = std::move(end);
        return ret;
    }
    static EncodedPacket fromComponents(uint8_t firstByte, SharedBuffer middle, SharedBuffer end) {
        EncodedPacket ret;
        ret.mType = Type::SingleByteWithLenAndMiddleAndEnd;
        auto varLength = encodeVarByteInt(end.size() + middle.size());
        ret.mMiddle = std::move(middle);
        ret.mPrelude.firstByte = firstByte;
        memcpy(ret.mPrelude.varLength, varLength.value, varLength.valueLength);
        ret.mPreludeLength = varLength.valueLength + 1;
        ret.mEnd = std::move(end);
        return ret;
    }
    void setDupFlag() {
        mPrelude.firstByte |= 0x08;
    }
    size_t constructIOVecs(size_t offset, iovec* iovecs);

    size_t fullSize() const {
        return mPreludeLength + mMiddle.size() + (mPacketId.has_value() ? sizeof(uint16_t) : 0) + mEnd.size();
    }
private:
    struct {
        uint8_t _padding[3];
        uint8_t firstByte{0};
        uint8_t varLength[4] = { 0 };
    } mPrelude;
    uint32_t mPreludeLength{1};
    SharedBuffer mMiddle;
    std::optional<uint16_t> mPacketId;
    SharedBuffer mEnd;
    Type mType{Type::Invalid};
};

enum class PacketSendState {
    FirstByte,
    Middle,
    PacketId,
    End,
    Done
};
struct InTransitEncodedPacket {
    EncodedPacket packet;
    size_t offset{0};
    bool isDone() const {
        return offset >= packet.fullSize();
    }
};


class MQTTPublishPacketBuilder {
public:
    // A reference to topic & payload is captured, so be cautious about lifetimes!
    MQTTPublishPacketBuilder(const std::string& topic, PayloadType payload, Retained retained, const PropertyList& properties);
    EncodedPacket getPacket(QoS qos, uint16_t packetId, MQTTVersion version);
private:
    const std::string& mTopic;
    PayloadType mPayload;
    Retained mRetained;
    std::optional<SharedBuffer> mPacketMiddle;
    std::unordered_map<MQTTVersion, SharedBuffer> mPacketPostlude;
    const PropertyList& mProperties;
};

}