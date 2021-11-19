#pragma once

#include <cstdint>
#include "TcpClientHandlerInterface.hpp"
#include <atomic>
#include <csignal>

namespace nioev {

class TcpServer {
    int mSockFd;
    std::atomic<bool> mShouldRun = true;
    std::atomic<pthread_t> mMainThreadHandle = 0;
public:
    explicit TcpServer(uint16_t port);
    ~TcpServer();

    void loop(TcpClientHandlerInterface& handler);
    void stopLoop();
};

}