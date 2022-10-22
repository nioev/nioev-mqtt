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

    void addRecentlyLoggedInClient(MQTTClientConnection* client);

    void suspendAllThreads();
    void resumeAllThreads();
private:
    void receiverThreadFunction(size_t threadId);
    void handlePacketsReceivedWhileConnecting(std::unique_lock<std::mutex>& recvDataLock, MQTTClientConnection& client);
private:
    std::vector<std::thread> mReceiverThreads;
    std::atomic<bool> mShouldQuit = false;
    int mEpollFd = -1;
    ApplicationState& mApp;
    std::shared_mutex mSuspendMutex;
    std::shared_mutex mSuspendMutex2;
    std::atomic<bool> mShouldSuspend{false};

    std::mutex mRecentlyLoggedInClientsMutex;
    std::vector<MQTTClientConnection*> mRecentlyLoggedInClients;
    std::atomic<bool> mRecentlyLoggedInClientsEmpty{true};
};

void handlePacketReceived(ApplicationState& app, MQTTClientConnection::ConnectionState state, MQTTClientConnection& client, const MQTTClientConnection::PacketReceiveData& recvData, std::unique_lock<std::mutex>& clientReceiveLock);

}