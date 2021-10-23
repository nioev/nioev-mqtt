#include "MQTTClientConnectionManager.hpp"
#include <spdlog/spdlog.h>

namespace nioev {

void MQTTClientConnectionManager::handleNewClientConnection(TcpClientConnection&& conn) {
    auto id = mClientIdCounter++;
    mClients.emplace(
        std::piecewise_construct,
        std::make_tuple(id),
        std::make_tuple(id, std::move(conn)));
}

}