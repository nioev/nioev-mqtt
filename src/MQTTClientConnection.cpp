#include "MQTTClientConnection.hpp"

#include <sys/socket.h>
#include <iostream>
#include <chrono>

#include <spdlog/spdlog.h>

namespace nioev {

MQTTClientConnection::MQTTClientConnection(ClientConnId id, TcpClientConnection&& conn)
: mId(id), mConn(std::move(conn)), mClientThread([this] {
      std::string threadName = "C-";
      threadName += std::to_string(mId);
      threadName += "-";
      threadName += mConn.getRemoteIp();
      pthread_setname_np(pthread_self(), threadName.c_str());
      loop();
  }) {

}

void MQTTClientConnection::loop() {
    spdlog::info("Thread for client {}:{} started", mConn.getRemoteIp(), mConn.getRemotePort());
    while(true) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

}