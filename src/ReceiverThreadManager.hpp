#pragma once

#include "MQTTClientConnection.hpp"
#include "ReceiverThreadManagerExternalBridgeInterface.hpp"
#include <thread>
#include <vector>

namespace nioev {

class ReceiverThreadManager {
public:
    explicit ReceiverThreadManager(ReceiverThreadManagerExternalBridgeInterface& bridge, uint threadCount);
    void addClientConnection(MQTTClientConnection& conn);
private:
    void receiverThreadFunction();
private:
    std::vector<std::thread> mReceiverThreads;
    std::atomic<bool> mShouldQuit = false;
    int mEpollFd = -1;
    std::vector<std::reference_wrapper<MQTTClientConnection>> mClients;
    ReceiverThreadManagerExternalBridgeInterface& mBridge;
};

}