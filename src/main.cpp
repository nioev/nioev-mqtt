#include "spdlog/spdlog.h"

#include "TcpServer.hpp"
#include "MQTTClientConnectionManager.hpp"

using namespace nioev;

int main() {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] %^[%-5l]%$ [%-15N] %v");

    MQTTClientConnectionManager clientManager;
    TcpServer server{5322};
    server.loop(clientManager);
}