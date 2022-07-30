#pragma once

#include "MQTTClientConnection.hpp"
#include "nioev/lib/Util.hpp"
#include <thread>
#include <vector>
#include "Forward.hpp"
#include <shared_mutex>

namespace nioev::mqtt {

using namespace nioev::lib;

class ClientThreadManager {
public:
    explicit ClientThreadManager(ApplicationState& bridge);
    ~ClientThreadManager();
    void addClientConnection(MQTTClientConnection& conn);
    void removeClientConnection(MQTTClientConnection& connection);

    void suspendAllThreads();
    void resumeAllThreads();
private:
    void receiverThreadFunction();
    void handlePacketReceived(MQTTClientConnection& client, const MQTTClientConnection::PacketReceiveData&, std::unique_lock<std::mutex>& clientReceiveLock);
    void protocolViolation(const std::string& reason);
private:
    std::vector<std::thread> mReceiverThreads;
    std::atomic<bool> mShouldQuit = false;
    int mEpollFd = -1;
    ApplicationState& mApp;
    std::shared_mutex mSuspendMutex;
    std::shared_mutex mSuspendMutex2;
    std::atomic<bool> mShouldSuspend{false};
};

}