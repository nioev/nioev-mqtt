#pragma once

#include "MQTTClientConnection.hpp"
#include "ReceiverThreadManager.hpp"
#include "ReceiverThreadManagerExternalBridgeInterface.hpp"
#include "TcpClientHandlerInterface.hpp"
#include <thread>
#include <unordered_map>
#include <vector>

namespace nioev {

class MQTTClientConnectionManager : public TcpClientHandlerInterface, public ReceiverThreadManagerExternalBridgeInterface {
private:
    std::unordered_map<int, MQTTClientConnection> mClients;
    ReceiverThreadManager mReceiverManager;
public:
    MQTTClientConnectionManager();
    void handleNewClientConnection(TcpClientConnection&&) override;
    MQTTClientConnection& getClient(int fd) override;
};

}