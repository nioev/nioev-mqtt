#pragma once

#include <cstdint>
#include "TcpClientHandlerInterface.hpp"

namespace nioev {

class TcpServer {
    int mSockFd;
public:
    explicit TcpServer(uint16_t port);
    ~TcpServer();

    void loop(TcpClientHandlerInterface& handler);
};

}