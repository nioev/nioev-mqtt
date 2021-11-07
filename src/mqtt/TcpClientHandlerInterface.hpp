#pragma once

#include <string>

namespace nioev {

class TcpClientConnection;

class TcpClientHandlerInterface {
public:
    virtual void handleNewClientConnection(TcpClientConnection&&) = 0;
};

}