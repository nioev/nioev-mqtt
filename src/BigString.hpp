#pragma once

#include "nioev/lib/Util.hpp"

namespace nioev {

using namespace nioev::lib;

template<uint Length>
class BigString final {
public:
    static_assert(Length <= 255);
    static_assert(Length + 1 >= sizeof(void*) + sizeof(size_t));
    BigString() {
        clear();
    }
    BigString(const char* data, size_t length) {
        set(data, length);
    }
    explicit BigString(const char* data) {
        set(data, strlen(data));
    }
    ~BigString() {
        clear();
    }
    BigString(const BigString<Length>&) = delete;
    void operator=(const BigString<Length>&&) = delete;

    BigString(BigString<Length>&& other) noexcept {
        operator=(std::move(other));
    }
    BigString& operator=(BigString<Length>&& other) noexcept {
        memcpy(mStr.data(), other.mStr.data(), other.mStr.size());
        isLong = other.isLong;
        mShortLength = other.mShortLength;
        other.clear();
        return *this;
    }

    void clear() {
        if(isLong) {
            delete[] c_str();
        }
        mStr[0] = 0;
        mShortLength = 0;
        isLong = false;
    }

    void set(const char* data, size_t length) {
        clear();
        if(length >= Length) {
            char* dataCpy = new char[length + 1];
            memcpy(dataCpy, data, length);
            dataCpy[length] = 0;
            memcpy(mStr.data(), &dataCpy, sizeof(void*));
            memcpy(mStr.data() + sizeof(void*), &length, sizeof(size_t));
            isLong = true;
        } else {
            mShortLength = length;
            memcpy(mStr.data(), data, length);
            mStr[length] = 0;
            isLong = false;
        }
    }

    [[nodiscard]] size_t size() const {
        if(isLong) {
            size_t ret;
            memcpy(&ret, mStr.data() + sizeof(char*), sizeof(size_t));
            return ret;
        } else {
            return mShortLength;
        }
    }

    [[nodiscard]] bool empty() const {
        return size() == 0;
    }

    [[nodiscard]] const char* c_str() const {
        if(isLong) {
            char* ret;
            memcpy(&ret, mStr.data(), sizeof(char*));
            return ret;
        } else {
            return mStr.data();
        }
    }
    [[nodiscard]] bool isShortOptimized() const {
        return !isLong;
    }
private:
    bool isLong = false;
    uint8_t mShortLength = 0;
    std::array<char, Length + 1> mStr;
};

}
