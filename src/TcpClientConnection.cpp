#include "TcpClientConnection.hpp"

#include <sys/socket.h>
#include <unistd.h>

namespace nioev {

TcpClientConnection::TcpClientConnection(int sockFd, std::string remoteIp, uint16_t remotePort)
: mSockFd(sockFd), mRemoteIp(std::move(remoteIp)), mRemotePort(remotePort) {

}
TcpClientConnection::~TcpClientConnection() {
    if(mSockFd != 0) {
        close(mSockFd);
    }
}
TcpClientConnection::TcpClientConnection(TcpClientConnection&& other) noexcept
: mSockFd(other.mSockFd), mRemoteIp(std::move(other.mRemoteIp)), mRemotePort(other.mRemotePort) {
    other.mRemotePort = 0;
    other.mRemoteIp = "";
    other.mSockFd = 0;

}

}
