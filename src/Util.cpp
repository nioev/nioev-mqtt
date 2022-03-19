#include "Util.hpp"

namespace nioev::util {



/*SharedBuffer::~SharedBuffer() {
    decRefCount();
}
SharedBuffer::SharedBuffer(const SharedBuffer& other) {
    operator=(other);
}
SharedBuffer& SharedBuffer::operator=(const SharedBuffer& other) {
    if(this == &other)
        return *this;
    mBuffer = other.mBuffer;
    mReserved = other.mReserved;
    mSize = other.mSize;
    mPacketId = other.mPacketId;
    incRefCount();
    return *this;
}
SharedBuffer::SharedBuffer(SharedBuffer&& other) noexcept {
    operator=(std::move(other));
}
SharedBuffer& SharedBuffer::operator=(SharedBuffer&& other) noexcept {
    mBuffer = other.mBuffer;
    mReserved = other.mReserved;
    mSize = other.mSize;
    mPacketId = other.mPacketId;
    other.mBuffer = nullptr;
    other.mReserved = 0;
    other.mSize = 0;
    other.mPacketId = 0;
    return *this;
}
std::atomic<int>& SharedBuffer::getRefCounter() {
    return *(std::atomic<int>*)mBuffer;
}
const std::atomic<int>& SharedBuffer::getRefCounter() const {
    return *(std::atomic<int>*)mBuffer;
}
void SharedBuffer::incRefCount() {
    if(mBuffer == nullptr)
        return;
    getRefCounter() += 1;
}
void SharedBuffer::decRefCount() {
    if(mBuffer == nullptr)
        return;
    auto refCount = getRefCounter().fetch_sub(1);
    if(refCount == 1) {
        getRefCounter().~atomic();
        free(mBuffer);
    }
    mBuffer = nullptr;
    mReserved = 0;
    mSize = 0;
    mPacketId = 0;
}
void SharedBuffer::resize(size_t newSize) {
    if(mReserved >= newSize) {
        mSize = newSize;
        return;
    }
    size_t newReserved = mReserved == 0 ? 32 : mReserved;
    while(newReserved < newSize) {
        newReserved *= 2;
    }
    auto newBuffer = (std::byte*)malloc(newReserved + sizeof(std::atomic<int>));
    assert(newBuffer);
    new(newBuffer) std::atomic<int>();
    if(mBuffer != nullptr) {
        memcpy(newBuffer + sizeof(std::atomic<int>), data(), mSize);
    }
    decRefCount();
    mBuffer = newBuffer;
    mReserved = newReserved;
    mSize = newSize;
    getRefCounter() = 1;
}
void SharedBuffer::append(const void* data, size_t size) {
    auto oldSize = mSize;
    resize(mSize + size);
    memcpy(this->data() + oldSize, data, size);
}
void SharedBuffer::insert(size_t index, const void* data, size_t size) {
    auto oldSize = mSize;
    resize(mSize + size);
    memmove(this->data() + index + size, this->data() + index, mSize - index);
    memcpy(this->data() + index, data, size);
}
SharedBuffer SharedBuffer::copy() const {
    if(getRefCounter() == 1) {
        return *this;
    }
    SharedBuffer ret;
    ret.mSize = mSize;
    ret.mReserved = mSize;
    ret.mBuffer = (std::byte*)malloc(mSize + sizeof(std::atomic<int>));
    memcpy(ret.mBuffer, data(), mSize);
    return ret;
}*/

}