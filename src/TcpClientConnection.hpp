#pragma once

#include <string>
#include <cstdint>
#include <vector>

namespace nioev {

class TcpClientConnection final {
public:
    TcpClientConnection(int sockFd, std::string remoteIp, uint16_t remotePort);
    ~TcpClientConnection();

    TcpClientConnection(const TcpClientConnection&) = delete;
    void operator=(const TcpClientConnection&) = delete;

    TcpClientConnection(TcpClientConnection&&) noexcept;
    TcpClientConnection& operator=(TcpClientConnection&&) = delete;

    [[nodiscard]] const std::string& getRemoteIp() const {
        return mRemoteIp;
    }
    [[nodiscard]] uint16_t getRemotePort() const {
        return mRemotePort;
    }
    [[nodiscard]] int getFd() const {
        return mSockFd;
    }

    std::vector<uint8_t> recv(uint length);
    std::vector<uint8_t> recvAllAvailableBytes();

private:
    int mSockFd;
    std::string mRemoteIp;
    uint16_t mRemotePort;
};

}
