#include "TcpServer.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdlib>
#include <string_view>
#include <arpa/inet.h>

#include "Util.hpp"
#include "TcpClientConnection.hpp"

#include "spdlog/spdlog.h"

namespace nioev {

TcpServer::TcpServer(uint16_t port) {
    struct sockaddr_in servaddr = { 0 };

    mSockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (mSockFd == -1) {
        spdlog::critical("Socket creation failed");
        exit(1);
    }
    spdlog::info("Socket created");
    int reuse = 1;
    if(setsockopt(mSockFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        spdlog::error("Failed to set SO_REUSEADDR on the socket");
    }

    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);

    if ((bind(mSockFd, (struct sockaddr*)&servaddr, sizeof(servaddr))) != 0) {
        spdlog::critical("Socket failed to bind to port {}", port);
        exit(2);
    }
    spdlog::info("Socket successfully bound");

    if ((listen(mSockFd, 10)) != 0) {
        spdlog::critical("Socket listen failed");
        exit(3);
    }
    spdlog::info("Socket listening");
}

TcpServer::~TcpServer() {
    close(mSockFd);
}

void TcpServer::loop(TcpClientHandlerInterface& handler) {
    spdlog::info("Entering TcpServer::loop");
    while(true) {
        struct sockaddr_in clientAddr;
        socklen_t len = sizeof(clientAddr);
        auto clientFd = accept(mSockFd, (struct sockaddr*)&clientAddr, &len);
        if(clientFd < 0) {
            spdlog::error("Failed to accept client: {}", util::errnoToString());
            continue;
        }

        char ipAsStr[32] = { 0 };
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipAsStr, 32);
        TcpClientConnection conn{clientFd, ipAsStr, clientAddr.sin_port};
        handler.handleNewClientConnection(std::move(conn));
    }
}

}