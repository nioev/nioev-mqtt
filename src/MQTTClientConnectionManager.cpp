#include "MQTTClientConnectionManager.hpp"
#include <spdlog/spdlog.h>

namespace nioev {

MQTTClientConnectionManager::MQTTClientConnectionManager()
: mReceiverManager(*this, 4), mSenderManager(*this, 2) {

}

void MQTTClientConnectionManager::handleNewClientConnection(TcpClientConnection&& conn) {
    spdlog::info("New Client {}:{}", conn.getRemoteIp(), conn.getRemotePort());
    std::lock_guard<std::shared_mutex> lock{mClientsMutex};
    int fd = conn.getFd();
    auto newClient = mClients.emplace(
        std::piecewise_construct,
        std::make_tuple(fd),
        std::make_tuple(std::move(conn)));
    mReceiverManager.addClientConnection(newClient.first->second);
    mSenderManager.addClientConnection(newClient.first->second);
}
std::pair<std::reference_wrapper<MQTTClientConnection>, std::shared_lock<std::shared_mutex>> MQTTClientConnectionManager::getClient(int fd) {
    std::shared_lock<std::shared_mutex> lock{mClientsMutex};
    return {mClients.at(fd), std::move(lock)};
}
void MQTTClientConnectionManager::sendData(MQTTClientConnection& conn, std::vector<uint8_t>&& data) {
    mSenderManager.sendData(conn, std::move(data));
}
void MQTTClientConnectionManager::notifyConnectionError(int connFd) {
    std::lock_guard<std::shared_mutex> lock{mClientsMutex};
    auto client = mClients.find(connFd);
    if(client == mClients.end()) {
        // Client was already deleted. This can happen if two receiver threads
        // get notified at the same time that a connection was closed and
        // both try to delete the connection at the same time.
        return;
    }
    spdlog::debug("Deleting connection {}", connFd);
    auto willMsg = client->second.getWill();
    if(willMsg) {
        // FIXME maybe we should do something with the QoS level? currently we always respect the subscriber's wish
        publishWithoutAcquiringLock(willMsg->topic, willMsg->msg);
    }
    mReceiverManager.removeClientConnection(client->second);
    mSenderManager.removeClientConnection(client->second);
    mSubscriptions.deleteAllSubscriptions(client->second);

    mClients.erase(client);
}
void MQTTClientConnectionManager::publish(const std::string& topic, const std::vector<uint8_t>& msg) {
    std::shared_lock<std::shared_mutex> lock{mClientsMutex};
    publishWithoutAcquiringLock(topic, msg);
}
void MQTTClientConnectionManager::publishWithoutAcquiringLock(const std::string& topic, const std::vector<uint8_t>& msg) {
#ifndef NDEBUG
    {
        std::string dataAsStr{msg.begin(), msg.end()};
        spdlog::info("Publishing on '{}' data '{}'", topic, dataAsStr);
    }
#endif
    mSubscriptions.forEachSubscriber(topic, [this, &topic, &msg] (auto& sub) {
        mSenderManager.sendPublish(sub.conn, topic, msg, sub.qos, Retained::No);
    });
}
void MQTTClientConnectionManager::addSubscription(MQTTClientConnection& conn, std::string&& topic, QoS qos) {
    std::shared_lock<std::shared_mutex> lock{mClientsMutex};
    mSubscriptions.addSubscription(conn, std::move(topic), qos, [&](const auto& topic, const auto& payload) {
        // this callback gets called for each retained message that we now need to publish
        mSenderManager.sendPublish(conn, topic, payload, qos, Retained::Yes);
    });
}
void MQTTClientConnectionManager::deleteSubscription(MQTTClientConnection& conn, const std::string& topic) {
    mSubscriptions.deleteSubscription(conn, topic);
}
void MQTTClientConnectionManager::retainMessage(std::string&& topic, std::vector<uint8_t>&& payload) {
    mSubscriptions.retainMessage(std::move(topic), std::move(payload));
}
}