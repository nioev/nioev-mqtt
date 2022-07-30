#pragma once

#include "nioev/lib/Util.hpp"

namespace nioev::mqtt {

using namespace nioev::lib;

template<typename T, uint Length>
class BigVector final {
public:
    static_assert(Length <= 255);
    static_assert(Length * sizeof(T) >= sizeof(T*) + sizeof(size_t));
    static_assert(std::is_standard_layout<T>() && std::is_trivial<T>());
    BigVector() {
        clear();
    }
    BigVector(const T* data, size_t length) {
        set(data, length);
    }
    ~BigVector() {
        clear();
    }
    BigVector(const BigVector<T, Length>&) = delete;
    void operator=(const BigVector<T, Length>&&) = delete;

    BigVector(BigVector<T, Length>&& other) noexcept {
        operator=(std::move(other));
    }
    BigVector& operator=(BigVector<T, Length>&& other) noexcept {
        memcpy(mBuffer.data(), other.mBuffer.data(), other.mBuffer.size());
        isLong = other.isLong;
        mShortLength = other.mShortLength;
        other.isLong = false;
        other.mShortLength = 0;
        return *this;
    }

    [[nodiscard]] const T& operator[](size_t i) const {
        assert(i <= size());
        return data()[i];
    }

    void clear() {
        if(isLong) {
            delete[] data();
        }
        mBuffer[0] = 0;
        isLong = false;
        mShortLength = 0;
    }

    void set(const T* data, size_t length) {
        if(isLong) {
            clear();
        }
        if(length > Length) {
            T* dataCpy = new T[length];
            memcpy(dataCpy, data, length * sizeof(T));
            memcpy(mBuffer.data(), &dataCpy, sizeof(void*));
            memcpy((uint8_t*)mBuffer.data() + sizeof(void*), &length, sizeof(size_t));
            isLong = true;
        } else {
            memcpy(mBuffer.data(), data, length * sizeof(T));
            mShortLength = length;
            isLong = false;
        }
    }

    [[nodiscard]] size_t size() const {
        if(isLong) {
            size_t ret;
            memcpy(&ret, (uint8_t*)mBuffer.data() + sizeof(char*), sizeof(size_t));
            return ret;
        } else {
            return mShortLength;
        }
    }

    [[nodiscard]] bool empty() const {
        return size() == 0;
    }

    [[nodiscard]] const T* data() const {
        if(empty())
            return nullptr;
        if(isLong) {
            T* ret;
            memcpy(&ret, mBuffer.data(), sizeof(T*));
            return ret;
        } else {
            return mBuffer.data();
        }
    }
    [[nodiscard]] bool isShortOptimized() const {
        return !isLong;
    }
private:
    bool isLong = false;
    uint8_t mShortLength = 0;
    std::array<T, Length> mBuffer;
};

}
