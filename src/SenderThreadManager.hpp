#pragma once

#include "Util.hpp"
#include <thread>
#include <queue>
#include <unordered_map>
#include <vector>
#include <list>

#include "Enums.hpp"
#include "Forward.hpp"
#include "SenderThreadManagerExternalBridgeInterface.hpp"

namespace nioev {

class SenderThreadManager {
public:
    explicit SenderThreadManager(SenderThreadManagerExternalBridgeInterface& bridge, uint threadCount);
    void addClientConnection(MQTTClientConnection& conn);
    void removeClientConnection(MQTTClientConnection& conn);


    // Build up a packet and send it immediately in this thread. If not all data can be send,
    // the rest will be send by the sender threads.
    void sendData(MQTTClientConnection& client, std::vector<uint8_t>&& data);
    void sendPublish(MQTTClientConnection& conn, const std::string& topic, const std::vector<uint8_t>& msg, QoS qos);



private:
    void senderThreadFunction();
private:
    std::vector<std::thread> mSenderThreads;
    std::atomic<bool> mShouldQuit = false;
    int mEpollFd = -1;
    SenderThreadManagerExternalBridgeInterface& mBridge;

    struct InitialSendTask {
        std::reference_wrapper<MQTTClientConnection> client;
        std::vector<uint8_t> data;
    };
};

}