#pragma once

#include "Forward.hpp"
#include <string>
#include <vector>
#include "Enums.hpp"
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>

namespace nioev {

struct AsyncPublishData {
    std::string topic;
    std::vector<uint8_t> payload;
    QoS qos;
    Retain retain;
};

class AsyncPublisher {
public:
    explicit AsyncPublisher(ApplicationState& app);
    ~AsyncPublisher();
    void publishAsync(AsyncPublishData&&);
private:
    void secondThreadFunc();

    ApplicationState& mApp;
    std::mutex mQueueMutex;
    std::queue<AsyncPublishData> mQueue;
    std::condition_variable mQueueCV;
    std::thread mThread;
    bool mShouldRun{true};
};

}