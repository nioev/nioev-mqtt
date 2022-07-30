#pragma once

#include <string>

namespace nioev::mqtt {

class TcpClientConnection;

class TcpClientHandlerInterface {
public:
    virtual void handleNewClientConnection(TcpClientConnection&&) = 0;
};

}