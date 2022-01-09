#pragma once

#include <atomic>
#include <cstdlib>
#include <string>
#include <cstring>
#include <arpa/inet.h>
#include <cstring>
#include <optional>

#include "spdlog/spdlog.h"
#include "Enums.hpp"

namespace nioev {

using uint = unsigned int;

constexpr const char* LOG_TOPIC = "$NIOEV/log";

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

// basically the same as std::shared_ptr<std::vector<uint8_t>> just with one less memory allocation
class SharedBuffer final {
public:
    SharedBuffer() = default;
    ~SharedBuffer();
    SharedBuffer(const SharedBuffer&);
    SharedBuffer& operator=(const SharedBuffer&);
    SharedBuffer(SharedBuffer&&) noexcept;
    SharedBuffer& operator=(SharedBuffer&&) noexcept;
    void resize(size_t newSize);

    [[nodiscard]] size_t size() const {
        return mSize;
    }
    [[nodiscard]] const uint8_t* data() const {
        if(mBuffer == nullptr)
            return nullptr;
        return (uint8_t*)(mBuffer + sizeof(std::atomic<int>));
    }
    [[nodiscard]] uint8_t* data() {
        if(mBuffer == nullptr)
            return nullptr;
        return (uint8_t*)(mBuffer + sizeof(std::atomic<int>));
    }
    void append(const void* data, size_t size);
    void insert(size_t index, const void* data, size_t size);
    SharedBuffer copy() const;
private:
    std::atomic<int>& getRefCounter();
    const std::atomic<int>& getRefCounter() const;
    void incRefCount();
    void decRefCount();
    std::byte* mBuffer { nullptr };
    size_t mReserved = 0;
    size_t mSize = 0;
};

class BinaryEncoder {
public:
    void encodeByte(uint8_t value) {
        mData.append(&value, 1);
    }
    void encode2Bytes(uint16_t value) {
        value = htons(value);
        mData.append((uint8_t*)&value, 2);
    }
    void encodeString(const std::string& str) {
        encode2Bytes(str.size());
        mData.append(str.c_str(), str.size());
    }
    void encodeBytes(const std::vector<uint8_t>& data) {
        mData.append(data.data(), data.size());
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
            mData.insert(offset, &encodeByte, 1);
            offset += 1;
        } while(packetLength > 0);
    }
    SharedBuffer&& moveData() {
        return std::move(mData);
    }

private:
    SharedBuffer mData;
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
static void splitString(const std::string& str, char delimiter, T callback) {
    std::string::size_type offset = 0, nextOffset = 0;
    do {
        nextOffset = str.find(delimiter, offset);
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
    splitString(topic, '/', [&] (const auto& actualPart) {
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
    splitString(topic, '/', [&parts](const std::string_view& part) {
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

// return e.g. ".js" or ".mp3"
inline std::string_view getFileExtension(const std::string& filename) {
    auto index = filename.find_last_of('.');
    if(index == std::string::npos) {
        return {};
    }
    return std::string_view{filename}.substr(index);
}

// return e.g. test for test.mp3
inline std::string_view getFileStem(const std::string& filename) {
    auto start = filename.find_last_of('/');
    if(start == std::string::npos) {
        start = 0;
    }
    auto end = filename.find_last_of('.');
    if(end == std::string::npos) {
        end = filename.size();
    }
    return std::string_view{filename}.substr(start, end - start);
}

}
}