#include "MQTTClientConnectionManager.hpp"
#include <spdlog/spdlog.h>

namespace nioev {

MQTTClientConnectionManager::MQTTClientConnectionManager()
: mReceiverManager(*this, 4) {

}

void MQTTClientConnectionManager::handleNewClientConnection(TcpClientConnection&& conn) {
    int fd = conn.getFd();
    auto newClient = mClients.emplace(
        std::piecewise_construct,
        std::make_tuple(fd),
        std::make_tuple(std::move(conn)));
    mReceiverManager.addClientConnection(newClient.first->second);
}
MQTTClientConnection& MQTTClientConnectionManager::getClient(int fd) {
    return mClients.at(fd);
}

}