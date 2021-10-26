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
}
std::pair<std::reference_wrapper<MQTTClientConnection>, std::shared_lock<std::shared_mutex>> MQTTClientConnectionManager::getClient(int fd) {
    return {mClients.at(fd), std::shared_lock<std::shared_mutex>{mClientsMutex}};
}
void MQTTClientConnectionManager::sendData(MQTTClientConnection& conn, std::vector<uint8_t>&& data) {
    mSenderManager.sendData(conn, std::move(data));
}

}