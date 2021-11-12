#pragma once

#include "MQTTClientConnection.hpp"
#include "Util.hpp"
#include <thread>
#include <vector>
#include "Forward.hpp"

namespace nioev {

class ClientThreadManager {
public:
    explicit ClientThreadManager(Application& bridge, uint threadCount);
    void addClientConnection(MQTTClientConnection& conn);
    void removeClientConnection(MQTTClientConnection& connection);
    void sendData(MQTTClientConnection& client, std::vector<uint8_t>&& data);
    void sendPublish(MQTTClientConnection& conn, const std::string& topic, const std::vector<uint8_t>& msg, QoS qos, Retained retain);
private:
    void receiverThreadFunction();
    void handlePacketReceived(MQTTClientConnection& client, const MQTTClientConnection::PacketReceiveData&);
    void protocolViolation();
private:
    std::vector<std::thread> mReceiverThreads;
    std::atomic<bool> mShouldQuit = false;
    int mEpollFd = -1;
    Application& mApp;
};

}