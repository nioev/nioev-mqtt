#pragma once

#include <string>
#include <cstring>
#include <arpa/inet.h>
#include <cstring>

#include "spdlog/spdlog.h"

namespace nioev {

using uint = unsigned int;

namespace util {

inline std::string errnoToString() {
    char buffer[1024] = { 0 };
#ifdef __linux__
    return strerror_r(errno, buffer, 1024);
#else
    strerror_r(errno, buffer, 1024);
    return buffer;
#endif
}

inline void throwErrno(std::string msg) {
    throw std::runtime_error{msg + ": " + errnoToString()};
}

class BinaryEncoder {
public:
    void encodeByte(uint8_t value) {
        mData.push_back(value);
    }
    void encode2Bytes(uint16_t value) {
        value = htons(value);
        mData.insert(mData.end(), (uint8_t*)&value, ((uint8_t*)&value) + 2);
    }
    void encodeString(const std::string& str) {
        encode2Bytes(str.size());
        mData.insert(mData.end(), str.begin(), str.end());
    }
    void encodeBytes(const std::vector<uint8_t>& data) {
        mData.insert(mData.end(), data.begin(), data.end());
    }
    void insertPacketLength() {
        uint32_t packetLength = mData.size() - 1; // exluding first byte
        do {
            uint8_t encodeByte = packetLength % 128;
            packetLength = packetLength / 128;
            // if there are more bytes to encode, set the upper bit
            if(packetLength > 0) {
                encodeByte |= 128;
            }
            mData.insert(mData.begin() + 1, encodeByte);
        } while(packetLength > 0);
    }
    std::vector<uint8_t>&& moveData() {
        return std::move(mData);
    }

private:
    std::vector<uint8_t> mData;
};

class BinaryDecoder {
public:
    explicit BinaryDecoder(const std::vector<uint8_t>& data, uint usableSize)
    : mData(data), mUsableSize(usableSize) {

    }
    std::string decodeString() {
        auto len = decode2Bytes();
        std::string ret{mData.begin() + mOffset, mData.begin() + mOffset + len};
        mOffset += len;
        return ret;
    }
    std::vector<uint8_t> decodeBytesWithPrefixLength() {
        auto len = decode2Bytes();
        std::vector<uint8_t> ret{mData.begin() + mOffset, mData.begin() + mOffset + len};
        mOffset += len;
        return ret;
    }
    uint8_t decodeByte() {
        return mData.at(mOffset++);
    }
    uint16_t decode2Bytes() {
        uint16_t len;
        memcpy(&len, mData.data() + mOffset, 2);
        len = ntohs(len);
        mOffset += 2;
        return len;
    }
    const uint8_t *getCurrentPtr() {
        return mData.data() + mOffset;
    }
    void advance(uint length) {
        mOffset += length;
    }
    std::vector<uint8_t> getRemainingBytes() {
        std::vector<uint8_t> ret(mData.begin() + mOffset, mData.end());
        mOffset = mData.size();
        return ret;
    }
    bool empty() {
        return mOffset >= mUsableSize;
    }
private:
    const std::vector<uint8_t>& mData;
    uint mOffset = 0;
    uint mUsableSize = 0;
};
}
}