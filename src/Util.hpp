#pragma once

#include <string>
#include <cstring>
#include <arpa/inet.h>
#include <cstring>
#include <optional>

#include "spdlog/spdlog.h"

namespace nioev {

using uint = unsigned int;

namespace util {

template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

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
    // this function takes about 33% of total calculation time - TODO optimize
    void insertPacketLength() {
        uint32_t packetLength = mData.size() - 1; // exluding first byte
        int offset = 1;
        do {
            uint8_t encodeByte = packetLength % 128;
            packetLength = packetLength / 128;
            // if there are more bytes to encode, set the upper bit
            if(packetLength > 0) {
                encodeByte |= 128;
            }
            mData.insert(mData.begin() + offset, encodeByte);
            offset += 1;
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

template<typename T>
class DestructWrapper final {
public:
    explicit DestructWrapper(T func)
    : mFunc(std::move(func)) {

    }
    ~DestructWrapper() {
        execute();
    }

    void execute() {
        if(mFunc) {
            mFunc.value()();
            mFunc.reset();
        }
    }

private:
    std::optional<T> mFunc;
};

static bool startsWith(const std::string& str, const std::string_view& prefix) {
    for(size_t i = 0; i < prefix.size(); ++i) {
        if(i >= str.size())
            return false;
        if(prefix.at(i) != str.at(i)) {
            return false;
        }
    }
    return true;
}

enum class IterationDecision {
    Continue,
    Stop
};

template<typename T>
static void splitString(const std::string& str, T callback) {
    std::string::size_type offset = 0, nextOffset = 0;
    do {
        nextOffset = str.find('/', offset);
        if(callback(std::string_view{str}.substr(offset, nextOffset - offset)) == IterationDecision::Stop) {
            break;
        }
        offset = nextOffset + 1;
    } while(nextOffset != std::string::npos);
}

static bool doesTopicMatchSubscription(const std::string& topic, const std::vector<std::string>& topicSplit) {
    size_t partIndex = 0;
    bool doesMatch = true;
    if((topic.at(0) == '$' && topicSplit.at(0).at(0) != '$') || (topic.at(0) != '$' && topicSplit.at(0).at(0) == '$')) {
        return false;
    }
    splitString(topic, [&] (const auto& actualPart) {
        if(topicSplit.size() <= partIndex) {
            doesMatch = false;
            return IterationDecision::Stop;
        }
        const auto& expectedPart = topicSplit.at(partIndex);
        if(actualPart == expectedPart || expectedPart == "+") {
            partIndex += 1;
            return IterationDecision::Continue;
        }
        if(expectedPart == "#") {
            partIndex = topicSplit.size();
            return IterationDecision::Stop;
        }
        doesMatch = false;
        return IterationDecision::Stop;
    });
    return doesMatch && partIndex == topicSplit.size();
}

inline std::vector<std::string> splitTopics(const std::string& topic) {
    std::vector<std::string> parts;
    splitString(topic, [&parts](const std::string_view& part) {
        parts.emplace_back(part);
        return util::IterationDecision::Continue;
    });
    return parts;
}

inline bool hasWildcard(const std::string& topic) {
    return std::any_of(topic.begin(), topic.end(), [](char c) {
        return c == '#' || c == '+';
    });
}

}
}