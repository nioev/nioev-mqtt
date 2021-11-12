#pragma once

#include "ClientThreadManager.hpp"
#include "MQTTClientConnection.hpp"
#include "SubscriptionsManager.hpp"
#include "TcpClientHandlerInterface.hpp"
#include <thread>
#include <unordered_map>
#include <vector>

namespace nioev {

class Application : public TcpClientHandlerInterface {
private:
    std::unordered_map<int, MQTTClientConnection> mClients;
    ClientThreadManager mReceiverManager;
    SubscriptionsManager mSubscriptions;

    void publishWithoutAcquiringLock(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain);
public:
    Application();
    void handleNewClientConnection(TcpClientConnection&&) override;
    std::pair<std::reference_wrapper<MQTTClientConnection>, std::shared_lock<std::shared_mutex>> getClient(int fd);
    void notifyConnectionError(int connFd);
    void publish(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain);
    void addSubscription(MQTTClientConnection& conn, std::string&& topic, QoS qos);
    void deleteSubscription(MQTTClientConnection& conn, const std::string& topic);

    std::shared_mutex mClientsMutex;
};

}