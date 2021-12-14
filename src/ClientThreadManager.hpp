#pragma once

#include "MQTTClientConnection.hpp"
#include "Util.hpp"
#include <thread>
#include <vector>
#include "Forward.hpp"

namespace nioev {

class ClientThreadManager {
public:
    explicit ClientThreadManager(ApplicationState& bridge, uint threadCount);
    ~ClientThreadManager();
    void addClientConnection(MQTTClientConnection& conn);
    void removeClientConnection(MQTTClientConnection& connection);

    void suspendAllThreads();
    void resumeAllThreads();
private:
    void receiverThreadFunction();
    void handlePacketReceived(MQTTClientConnection& client, const MQTTClientConnection::PacketReceiveData&, std::unique_lock<std::mutex>& clientReceiveLock);
    void protocolViolation();
private:
    std::vector<std::thread> mReceiverThreads;
    std::atomic<bool> mShouldQuit = false;
    int mEpollFd = -1;
    ApplicationState& mApp;
    std::atomic<bool> mShouldSuspend = false;
    std::atomic<int> mSuspendedThreads = 0;
};

}