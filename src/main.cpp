#include "spdlog/spdlog.h"

#include "TcpServer.hpp"
#include "MQTTClientConnectionManager.hpp"
#include <csignal>

using namespace nioev;

int main() {
    signal(SIGUSR1, [](int){});
    spdlog::set_level(spdlog::level::trace);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] %^[%-5l]%$ [%-15N] %v");

    MQTTClientConnectionManager clientManager;
    TcpServer server{1883};
    spdlog::info("TcpServer started");
    server.loop(clientManager);
}