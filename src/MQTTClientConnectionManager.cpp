#include "MQTTClientConnectionManager.hpp"
#include <spdlog/spdlog.h>

namespace nioev {

MQTTClientConnectionManager::MQTTClientConnectionManager()
: mReceiverManager(*this, 4), mSenderManager(*this, 4) {

}

void MQTTClientConnectionManager::handleNewClientConnection(TcpClientConnection&& conn) {
    int fd = conn.getFd();
    auto newClient = mClients.emplace(
        std::piecewise_construct,
        std::make_tuple(fd),
        std::make_tuple(std::move(conn)));
    mReceiverManager.addClientConnection(newClient.first->second);
    mSenderManager.addClientConnection(newClient.first->second);
}
std::pair<std::reference_wrapper<MQTTClientConnection>, std::shared_lock<std::shared_mutex>> MQTTClientConnectionManager::getClient(int fd) {
    return {mClients.at(fd), std::shared_lock<std::shared_mutex>{mClientsMutex}};
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
    spdlog::info("Deleting connection {}", connFd);
    mReceiverManager.removeClientConnection(client->second);
    mSenderManager.removeClientConnection(client->second);

    mClients.erase(client);
}

}