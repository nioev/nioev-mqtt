#include "TcpClientConnection.hpp"

#include <sys/socket.h>
#include <unistd.h>
#include "Util.hpp"

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
std::vector<uint8_t> TcpClientConnection::recv(uint length) {
    std::vector<uint8_t> buffer(length);
    int result = ::recv(mSockFd, buffer.data(), buffer.size(), MSG_DONTWAIT);
    if(result <= 0) {
        if(errno == EWOULDBLOCK || errno == EAGAIN) {
            return {};
        }
        // connection dropped or so
        util::throwErrno("recv()");
    }
    buffer.resize(result);
    return buffer;
}
std::vector<uint8_t> TcpClientConnection::recvAllAvailableBytes() {
    // TODO use the actual pipe size
    return recv(64 * 1024);
}

}
