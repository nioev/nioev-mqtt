#pragma once

#include "Forward.hpp"
#include <shared_mutex>

namespace nioev {

class ReceiverThreadManagerExternalBridgeInterface {
public:
    virtual std::pair<std::reference_wrapper<MQTTClientConnection>, std::shared_lock<std::shared_mutex>> getClient(int fd) = 0;
    virtual void sendData(MQTTClientConnection& conn, std::vector<uint8_t>&& data) = 0;
    virtual void notifyConnectionError(int connFd) = 0;
    virtual void publish(const std::string& topic, std::vector<uint8_t>& msg) = 0;
    virtual void addSubscription(MQTTClientConnection& conn, std::string&& topic, QoS qos) = 0;
    virtual void deleteSubscription(MQTTClientConnection& conn, const std::string& topic) = 0;
};

}