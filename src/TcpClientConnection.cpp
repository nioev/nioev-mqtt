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
        if(close(mSockFd) < 0) {
            spdlog::error("close(): {}", util::errnoToString());
        }
    }
}
TcpClientConnection::TcpClientConnection(TcpClientConnection&& other) noexcept
: mSockFd(other.mSockFd), mRemoteIp(std::move(other.mRemoteIp)), mRemotePort(other.mRemotePort) {
    other.mRemotePort = 0;
    other.mRemoteIp = "";
    other.mSockFd = 0;

}
uint TcpClientConnection::recv(std::vector<uint8_t>& buffer) {
    assert(buffer.size() > 0);
    auto result = ::recv(mSockFd, buffer.data(), buffer.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if(result == 0) {
        throw std::runtime_error{"recv(): Remote closed connection"};
    }
    if(result < 0) {
        if(errno == EWOULDBLOCK || errno == EAGAIN) {
            return 0;
        }
        // connection dropped or so
        util::throwErrno("recv()");
    }
    return result;
}
uint TcpClientConnection::send(const uint8_t* data, uint len) {
    assert(len > 0);
    auto result = ::send(mSockFd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if(result == 0) {
        throw std::runtime_error{"send(): Remote closed connection"};
    }
    if(result < 0) {
        if(errno == EWOULDBLOCK || errno == EAGAIN) {
            return 0;
        }
        util::throwErrno("send()");
    }
    return result;
}

}
