#pragma once

#include "Forward.hpp"
#include <string>
#include <vector>
#include "nioev/lib/Enums.hpp"
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>
#include <condition_variable>
#include "nioev/lib/GenServer.hpp"

namespace nioev::mqtt {

using namespace nioev::lib;

struct AsyncPublishData {
    std::string topic;
    std::vector<uint8_t> payload;
    QoS qos;
    Retain retain;
};

class AsyncPublisher : public GenServer<AsyncPublishData> {
public:
    explicit AsyncPublisher(ApplicationState& app);
private:
    void handleTask(AsyncPublishData&&) override;
    ApplicationState& mApp;
};

}