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
    void removeClientConnection(MQTTClientConnection& connection);

private:
    void receiverThreadFunction();
    void handlePacketReceived(MQTTClientConnection& client, const MQTTClientConnection::PacketReceiveData&);
    void protocolViolation();
private:
    std::vector<std::thread> mReceiverThreads;
    std::atomic<bool> mShouldQuit = false;
    int mEpollFd = -1;
    ReceiverThreadManagerExternalBridgeInterface& mBridge;
};

}