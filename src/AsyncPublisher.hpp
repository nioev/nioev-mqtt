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
#include "nioev/lib/Util.hpp"

namespace nioev::mqtt {

using namespace nioev::lib;


class AsyncPublisher : public GenServer<MQTTPacket> {
public:
    explicit AsyncPublisher(ApplicationState& app);
private:
    void handleTask(MQTTPacket&&) override;
    ApplicationState& mApp;
};

}