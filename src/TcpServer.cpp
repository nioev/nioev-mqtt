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
#include "poll.h"

#include "nioev/lib/Util.hpp"
#include "TcpClientConnection.hpp"

#include "spdlog/spdlog.h"

namespace nioev {

using namespace nioev::lib;

TcpServer::TcpServer(uint16_t port, TcpClientHandlerInterface& handler) {
    struct sockaddr_in servaddr = { 0 };

    mSockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (mSockFd == -1) {
        spdlog::critical("Socket creation failed");
        exit(1);
    }
    spdlog::trace("Socket created");
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
    spdlog::trace("Socket successfully bound");

    if ((listen(mSockFd, 10)) != 0) {
        spdlog::critical("Socket listen failed");
        exit(3);
    }
    spdlog::trace("Socket listening");

    mLoopThread.emplace([this, &handler] {
        pthread_setname_np(pthread_self(), "TcpServer");
        loopThreadFunc(handler);
    });
}

TcpServer::~TcpServer() {
    requestStop();
    close(mSockFd);
}

void TcpServer::loopThreadFunc(TcpClientHandlerInterface& handler) {
    while(mShouldRun) {
        struct sockaddr_in clientAddr;
        socklen_t len = sizeof(clientAddr);

        struct pollfd pollInfo;
        pollInfo.fd = mSockFd;
        pollInfo.events = POLLIN;
        if(poll(&pollInfo, 1, -1) < 0) {
            if(!mShouldRun) {
                spdlog::info("Safely aborted TcpServer accept loop");
                return;
            }
            spdlog::error("poll(): {}", errnoToString());
            continue;
        }
        if(!mShouldRun) {
            return;
        }
        auto clientFd = accept4(mSockFd, (struct sockaddr*)&clientAddr, &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if(clientFd < 0) {
            spdlog::error("Failed to accept client: {}", errnoToString());
            continue;
        }

        char ipAsStr[32] = { 0 };
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipAsStr, 32);
        TcpClientConnection conn{clientFd, ipAsStr, clientAddr.sin_port};
        handler.handleNewClientConnection(std::move(conn));
    }
}
void TcpServer::requestStop() {
    if(!mShouldRun) {
        return;
    }
    mShouldRun = false;
    pthread_kill(mLoopThread->native_handle(), SIGUSR1);
}
void TcpServer::join() {
    mLoopThread->join();
}

}