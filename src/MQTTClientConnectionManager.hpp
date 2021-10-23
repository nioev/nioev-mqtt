#pragma once

#include "TcpClientHandlerInterface.hpp"
#include "MQTTClientConnection.hpp"
#include <unordered_map>

namespace nioev {

class MQTTClientConnectionManager : public TcpClientHandlerInterface {
private:
    std::unordered_map<ClientConnId, MQTTClientConnection> mClients;
    ClientConnId mClientIdCounter = 0;
public:
    void handleNewClientConnection(TcpClientConnection&&) override;

};

}