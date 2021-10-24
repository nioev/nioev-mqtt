#include "MQTTClientConnection.hpp"

#include <sys/socket.h>
#include <iostream>
#include <chrono>

#include <spdlog/spdlog.h>

namespace nioev {

MQTTClientConnection::MQTTClientConnection(TcpClientConnection&& conn)
: mConn(std::move(conn)) {
}
}