#pragma once

#include <cstdint>
#include "TcpClientHandlerInterface.hpp"
#include <atomic>
#include <csignal>
#include <thread>
#include <optional>

namespace nioev {

class TcpServer {
    int mSockFd;
    std::atomic<bool> mShouldRun = true;
    std::optional<std::thread> mLoopThread;

    void loopThreadFunc(TcpClientHandlerInterface& handler);
public:
    explicit TcpServer(uint16_t port, TcpClientHandlerInterface& handler);
    ~TcpServer();
    void requestStop();
    void join();
};

}