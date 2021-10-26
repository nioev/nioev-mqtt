#pragma once

#include "Forward.hpp"
#include <shared_mutex>

namespace nioev {

class SenderThreadManagerExternalBridgeInterface {
public:
    virtual std::pair<std::reference_wrapper<MQTTClientConnection>, std::shared_lock<std::shared_mutex>> getClient(int fd) = 0;
};

}