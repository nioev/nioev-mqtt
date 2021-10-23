#pragma once

#include <string>
#include <cstdint>
#include <thread>
#include <mutex>

#include "TcpClientConnection.hpp"

namespace nioev {

using ClientConnId = int32_t;

class MQTTClientConnection {
public:
    MQTTClientConnection(ClientConnId id, TcpClientConnection&& tcpClient);

private:
    void loop();

private:
    ClientConnId mId;
    std::mutex mClientMutex;
    std::thread mClientThread;
    TcpClientConnection mConn;
};


}