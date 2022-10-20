#pragma once
#include <atomic>
#include <cstdint>

namespace nioev::mqtt {

class TaskQueueRefCount {
public:
    void incTaskQueueRefCount() {
        mRefCount++;
    }
    bool decTaskQueueRefCount() {
        mRefCount--;
        return mRefCount == 0;
    }
    uint32_t getTaskQueueRefCount() {
        return mRefCount;
    }

private:
    std::atomic<uint32_t> mRefCount = 0;
};

}