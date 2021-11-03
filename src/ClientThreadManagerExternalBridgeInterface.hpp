#pragma once

#include "Forward.hpp"
#include <shared_mutex>
#include <vector>
#include "Enums.hpp"

namespace nioev {

class ClientThreadManagerExternalBridgeInterface {
public:
    virtual std::pair<std::reference_wrapper<MQTTClientConnection>, std::shared_lock<std::shared_mutex>> getClient(int fd) = 0;
    virtual void sendData(MQTTClientConnection& conn, std::vector<uint8_t>&& data) = 0;
    virtual void notifyConnectionError(int connFd) = 0;
    virtual void publish(const std::string& topic, const std::vector<uint8_t>& msg) = 0;
    virtual void addSubscription(MQTTClientConnection& conn, std::string&& topic, QoS qos) = 0;
    virtual void deleteSubscription(MQTTClientConnection& conn, const std::string& topic) = 0;
    virtual void retainMessage(std::string&& topic, std::vector<uint8_t>&& payload) = 0;
};

}