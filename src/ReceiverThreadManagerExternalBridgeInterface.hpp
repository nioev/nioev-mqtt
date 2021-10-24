#pragma once

#include "Forward.hpp"

namespace nioev {

class ReceiverThreadManagerExternalBridgeInterface {
public:
    virtual MQTTClientConnection& getClient(int fd) = 0;
};

}