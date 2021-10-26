#pragma once

#include <string>
#include <cstdint>
#include <thread>
#include <mutex>

#include "TcpClientConnection.hpp"
#include "Enums.hpp"

namespace nioev {

class MQTTClientConnection {
public:
    MQTTClientConnection(TcpClientConnection&& conn);

    [[nodiscard]] TcpClientConnection& getTcpClient() {
        return mConn;
    }
    enum class ConnectionState {
        INITIAL,
        CONNECTED,
        INVALID_PROTOCOL_VERSION
    };
    enum class PacketReceiveState {
        IDLE,
        RECEIVING_VAR_LENGTH,
        RECEIVING_DATA
    };

    struct PacketReceiveData {
        std::vector<uint8_t> currentReceiveBuffer;
        PacketReceiveState recvState = PacketReceiveState::IDLE;
        MQTTMessageType messageType = MQTTMessageType::Invalid;
        uint32_t packetLength = 0;
        uint32_t multiplier = 1;
        uint8_t firstByte = 0;
    };
    std::pair<std::reference_wrapper<PacketReceiveData>, std::unique_lock<std::mutex>> getRecvData() {
        return {mRecvData, std::unique_lock<std::mutex>{mMutex}};
    }
    [[nodiscard]] ConnectionState getState() {
        return mState;
    }
    void setState(ConnectionState newState) {
        mState = newState;
    }
private:
    std::mutex mMutex;
    TcpClientConnection mConn;
    PacketReceiveData mRecvData;
    ConnectionState mState = ConnectionState::INITIAL;
};


}