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
    PacketReceiveData& getRecvData() {
        return mRecvData;
    }
private:
    TcpClientConnection mConn;
    PacketReceiveData mRecvData;
};


}