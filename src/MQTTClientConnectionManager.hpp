#pragma once

#include "MQTTClientConnection.hpp"
#include "ReceiverThreadManager.hpp"
#include "ReceiverThreadManagerExternalBridgeInterface.hpp"
#include "SenderThreadManager.hpp"
#include "SenderThreadManagerExternalBridgeInterface.hpp"
#include "TcpClientHandlerInterface.hpp"
#include <thread>
#include <unordered_map>
#include <vector>
#include "SubscriptionsManager.hpp"

namespace nioev {

class MQTTClientConnectionManager : public TcpClientHandlerInterface, public ReceiverThreadManagerExternalBridgeInterface, public SenderThreadManagerExternalBridgeInterface {
private:
    std::unordered_map<int, MQTTClientConnection> mClients;
    ReceiverThreadManager mReceiverManager;
    SenderThreadManager mSenderManager;
    SubscriptionsManager mSubscriptions;

    void publishWithoutAcquiringLock(const std::string& topic, std::vector<uint8_t>& msg, QoS qos);
public:
    MQTTClientConnectionManager();
    void handleNewClientConnection(TcpClientConnection&&) override;
    std::pair<std::reference_wrapper<MQTTClientConnection>, std::shared_lock<std::shared_mutex>> getClient(int fd) override;
    void sendData(MQTTClientConnection& conn, std::vector<uint8_t>&& data) override;
    void notifyConnectionError(int connFd) override;
    void publish(const std::string& topic, std::vector<uint8_t>& msg, QoS qos) override;
    void addSubscription(MQTTClientConnection& conn, std::string&& topic, QoS qos) override;
    void deleteSubscription(MQTTClientConnection& conn, const std::string& topic) override;

    std::shared_mutex mClientsMutex;
};

}