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
        auto len = decode2Bytes();
        std::string ret{mData.begin() + mOffset, mData.begin() + mOffset + len};
        mOffset += len;
        return ret;
    }
    uint8_t decodeByte() {
        return mData.at(mOffset++);
    }
    uint16_t decode2Bytes() {
        uint16_t len;
        memcpy(&len, mData.data(), 2);
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
private:
    const std::vector<uint8_t>& mData;
    uint mOffset = 0;
};
}
}