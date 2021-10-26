#pragma once

#include "MQTTClientConnection.hpp"
#include "ReceiverThreadManager.hpp"
#include "ReceiverThreadManagerExternalBridgeInterface.hpp"
#include "SenderThreadManagerExternalBridgeInterface.hpp"
#include "TcpClientHandlerInterface.hpp"
#include <thread>
#include <unordered_map>
#include <vector>

namespace nioev {

class MQTTClientConnectionManager : public TcpClientHandlerInterface, public ReceiverThreadManagerExternalBridgeInterface, public SenderThreadManagerExternalBridgeInterface {
private:
    std::unordered_map<int, MQTTClientConnection> mClients;
    ReceiverThreadManager mReceiverManager;
public:
    MQTTClientConnectionManager();
    void handleNewClientConnection(TcpClientConnection&&) override;
    std::pair<std::reference_wrapper<MQTTClientConnection>, std::shared_lock<std::shared_mutex>> getClient(int fd) override;
    void sendData(MQTTClientConnection& conn, std::vector<uint8_t>&& data) override;

    std::shared_mutex mClientsMutex;
};

}