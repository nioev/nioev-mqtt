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
    return strerror_r(errno, buffer, 1024);
}

inline void throwErrno(std::string msg) {
    throw std::runtime_error{msg + ": " + errnoToString()};
}

class BinaryDecoder {
public:
    explicit BinaryDecoder(const std::vector<uint8_t>& data)
    : mData(data) {

    }
    std::string decodeString() {
        uint16_t len;
        memcpy(&len, mData.data(), 2);
        len = ntohs(len);
        std::string ret{mData.begin() + mOffset + 2, mData.begin() + mOffset + 2 + len};
        mOffset += 2 + len;
        return ret;
    }
    uint8_t decodeByte() {
        return mData.at(mOffset++);
    }
    const uint8_t *getCurrentPtr() {
        return mData.data() + mOffset;
    }
    void advance(uint length) {
        mOffset += length;
    }
private:
    const std::vector<uint8_t>& mData;
    uint mOffset = 0;
};
}
}