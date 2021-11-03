#pragma once

#include "ClientThreadManager.hpp"
#include "ClientThreadManagerExternalBridgeInterface.hpp"
#include "MQTTClientConnection.hpp"
#include "SubscriptionsManager.hpp"
#include "TcpClientHandlerInterface.hpp"
#include <thread>
#include <unordered_map>
#include <vector>

namespace nioev {

class MQTTClientConnectionManager : public TcpClientHandlerInterface, public ClientThreadManagerExternalBridgeInterface {
private:
    std::unordered_map<int, MQTTClientConnection> mClients;
    ClientThreadManager mReceiverManager;
    SubscriptionsManager mSubscriptions;

    void publishWithoutAcquiringLock(const std::string& topic, const std::vector<uint8_t>& msg);
public:
    MQTTClientConnectionManager();
    void handleNewClientConnection(TcpClientConnection&&) override;
    std::pair<std::reference_wrapper<MQTTClientConnection>, std::shared_lock<std::shared_mutex>> getClient(int fd) override;
    void sendData(MQTTClientConnection& conn, std::vector<uint8_t>&& data) override;
    void notifyConnectionError(int connFd) override;
    void publish(const std::string& topic, const std::vector<uint8_t>& msg) override;
    void addSubscription(MQTTClientConnection& conn, std::string&& topic, QoS qos) override;
    void deleteSubscription(MQTTClientConnection& conn, const std::string& topic) override;
    void retainMessage(std::string&& topic, std::vector<uint8_t>&& payload) override;

    std::shared_mutex mClientsMutex;
};

}