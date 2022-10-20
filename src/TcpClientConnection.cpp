#include "TcpClientConnection.hpp"

#include "nioev/lib/Util.hpp"
#include "spdlog/spdlog.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace nioev::mqtt {

using namespace nioev::lib;

TcpClientConnection::TcpClientConnection(int sockFd, std::string remoteIp, uint16_t remotePort)
: mSockFd(sockFd), mRemoteIp(std::move(remoteIp)), mRemotePort(remotePort) {

}
TcpClientConnection::~TcpClientConnection() {
    close();
}
TcpClientConnection::TcpClientConnection(TcpClientConnection&& other) noexcept
: mRemoteIp(std::move(other.mRemoteIp)), mRemotePort(other.mRemotePort) {
    mSockFd = other.mSockFd.load();
    other.mRemotePort = 0;
    other.mRemoteIp = "";
    other.mSockFd = -1;
}
uint TcpClientConnection::recv(std::vector<uint8_t>& buffer) {
    assert(buffer.size() > 0);
    auto fd = mSockFd.load();
    if(fd == -1)
        throwErrno("recv()");
    auto result = ::recv(fd, buffer.data(), buffer.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if(result == 0) {
        throw CleanDisconnectException{};
    }
    if(result < 0) {
        if(errno == EWOULDBLOCK || errno == EAGAIN) {
            return 0;
        }
        // connection dropped or so
        throwErrno("recv()");
    }
    return result;
}
uint TcpClientConnection::send(const uint8_t* data, uint len) {
    assert(len > 0);
    auto fd = mSockFd.load();
    if(fd == -1)
        throwErrno("recv()");
    auto result = ::send(fd, data, len, MSG_NOSIGNAL | MSG_DONTWAIT);
    if(result == 0) {
        throw CleanDisconnectException{};
    }
    if(result < 0) {
        if(errno == EWOULDBLOCK || errno == EAGAIN) {
            return 0;
        }
        throwErrno("send()");
    }
    return result;
}
void TcpClientConnection::close() {
    int expected = mSockFd.load();
    if(expected >= 0 && mSockFd.compare_exchange_strong(expected, -1)) {
        if(::close(expected) < 0) {
            spdlog::error("close({}): {}", expected, errnoToString());
        }
    }
}
uint TcpClientConnection::sendScatter(InTransitEncodedPacket* packets, size_t encodedPacketCount) {
    assert(encodedPacketCount > 0);
    encodedPacketCount = (std::min)(encodedPacketCount, (size_t)UIO_MAXIOV / 4);
    auto fd = mSockFd.load();
    if(fd == -1)
        throwErrno("recv()");
    msghdr scatterMessage = { 0 };
    size_t vecsOffset = 0;
    iovec vecs[encodedPacketCount * 4];
    memset(&vecs, 0, sizeof(vecs));

    for(size_t i = 0; i < encodedPacketCount; ++i) {
        vecsOffset += packets[i].packet.constructIOVecs(packets[i].offset, vecs + vecsOffset);
    }
    if(vecsOffset >= UIO_MAXIOV) {
        spdlog::error("Over max: {}", vecsOffset);
    }
    scatterMessage.msg_iov = vecs;
    scatterMessage.msg_iovlen = vecsOffset;
    ssize_t result = ::sendmsg(fd, &scatterMessage, MSG_NOSIGNAL | MSG_DONTWAIT);
    if(result == 0) {
        throw CleanDisconnectException{};
    }
    if(result < 0) {
        if(errno == EWOULDBLOCK || errno == EAGAIN) {
            return 0;
        }
        throwErrno("send()");
    }
    for(size_t i = 0; i < encodedPacketCount; ++i) {
        auto remainingPacketSize = packets[i].packet.fullSize() - packets[i].offset;
        if(remainingPacketSize <= result) {
            packets[i].offset = packets[i].packet.fullSize();
            result -= remainingPacketSize;
        } else {
            packets[i].offset += result;
            result = 0;
            break;
        }
    }
    assert(result == 0);
    return result;
}

}
