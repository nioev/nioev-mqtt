#pragma once

#include <string>
#include <cstdint>
#include <thread>
#include <mutex>
#include <optional>
#include <queue>

#include "Forward.hpp"
#include "TcpClientConnection.hpp"
#include "Enums.hpp"
#include "Subscriber.hpp"

namespace nioev {

class MQTTClientConnection final : public Subscriber, public std::enable_shared_from_this<MQTTClientConnection> {
public:
    MQTTClientConnection(TcpClientConnection&& conn)
    : mConn(std::move(conn)) {
        mClientId = mConn.getRemoteIp() + ":" + std::to_string(mConn.getRemotePort());
    }

    [[nodiscard]] TcpClientConnection& getTcpClient() {
        return mConn;
    }

    enum class ConnectionState {
        INITIAL,
        CONNECTED,
        INVALID_PROTOCOL_VERSION
    };
    [[nodiscard]] ConnectionState getState() {
        std::lock_guard<std::mutex> lock{mRemaingingMutex};
        return mState;
    }
    void setState(ConnectionState newState) {
        std::lock_guard<std::mutex> lock{mRemaingingMutex};
        mState = newState;
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
        int64_t lastDataReceivedTimestamp = std::chrono::steady_clock::now().time_since_epoch().count();
    };
    std::pair<std::reference_wrapper<PacketReceiveData>, std::unique_lock<std::mutex>> getRecvData() {
        std::unique_lock<std::mutex> lock{mRecvMutex};
        return {mRecvData, std::move(lock)};
    }

    struct SendTask {
        std::vector<uint8_t> data;
        uint offset = 0;
    };
    std::pair<std::reference_wrapper<std::queue<SendTask>>, std::unique_lock<std::mutex>> getSendTasks() {
        std::unique_lock<std::mutex> lock{mSendMutex};
        return {mSendTasks, std::move(lock)};
    }

    void setWill(std::string&& topic, std::vector<uint8_t>&& msg, QoS qos, Retain retain) {
        std::lock_guard<std::mutex> lock{mRemaingingMutex};
        mWill.emplace();
        mWill->topic = std::move(topic);
        mWill->msg = std::move(msg);
        mWill->qos = qos;
        mWill->retain = retain;
    }
    void discardWill() {
        std::lock_guard<std::mutex> lock{mRemaingingMutex};
        mWill.reset();
    }
    auto moveWill() {
        std::lock_guard<std::mutex> lock{mRemaingingMutex};
        return std::move(mWill);
    }
    void setPersistentState(PersistentClientState* newState) {
        std::unique_lock lock{mRemaingingMutex};
        mPersistentState = newState;
    }
    std::pair<PersistentClientState*&, std::unique_lock<std::mutex>> getPersistentState() {
        std::unique_lock lock{mRemaingingMutex};
        return {mPersistentState, std::move(lock)};
    }
    void notifyConnecionError() {
        mShouldBeDisconnected = true;
    }
    [[nodiscard]] bool shouldBeDisconnected() const {
        return mShouldBeDisconnected;
    }
    void setKeepAliveIntervalSeconds(uint16_t keepAlive) {
        mKeepAliveIntervalSeconds = keepAlive;
    }
    [[nodiscard]] uint16_t getKeepAliveIntervalSeconds() const {
        return mKeepAliveIntervalSeconds;
    }
    TcpClientConnection&& moveTcpClient() {
        return std::move(mConn);
    }

    void setClientId(std::string clientId) {
        std::lock_guard lock{mRemaingingMutex};
        mClientId = std::move(clientId);
    }
    const std::string& getClientId() {
        std::lock_guard lock{mRemaingingMutex};
        return mClientId;
    }

    auto makeShared() {
        return shared_from_this();
    }

    void sendData(std::vector<uint8_t>&& bytes) {
        try {
            uint totalBytesSent = 0;
            uint bytesSent = 0;
            auto [sendTasksRef, sendLock] = getSendTasks();
            auto& sendTasks = sendTasksRef.get();
            if(sendTasks.empty()) {
                do {
                    bytesSent = getTcpClient().send(bytes.data() + totalBytesSent, bytes.size() - totalBytesSent);
                    totalBytesSent += bytesSent;
                } while(bytesSent > 0 && totalBytesSent < bytes.size());
            }
            //spdlog::warn("Bytes sent: {}, Total bytes sent: {}", bytesSent, totalBytesSent);

            if(totalBytesSent < bytes.size()) {
                // TODO implement maximum queue depth
                sendTasks.emplace(MQTTClientConnection::SendTask{ std::move(bytes), totalBytesSent });
            }
        } catch(std::exception& e) {
            spdlog::error("Error while sending data: {}", e.what());
            notifyConnecionError();
        }
    }
    void publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained) override {
        util::BinaryEncoder encoder;
        uint8_t firstByte = static_cast<uint8_t>(QoS::QoS0) << 1; //FIXME use actual qos
        // TODO retain, dup
        if(retained == Retained::Yes) {
            firstByte |= 1;
        }
        firstByte |= static_cast<uint8_t>(MQTTMessageType::PUBLISH) << 4;
        encoder.encodeByte(firstByte);
        encoder.encodeString(topic);
        if(qos != QoS::QoS0) {
            // TODO add id
            assert(false);
        }
        encoder.encodeBytes(payload);
        encoder.insertPacketLength();
        sendData(encoder.moveData());
    }
private:
    TcpClientConnection mConn;

    std::mutex mRecvMutex;
    PacketReceiveData mRecvData;

    std::mutex mSendMutex;
    std::queue<SendTask> mSendTasks;

    std::mutex mRemaingingMutex;
    ConnectionState mState = ConnectionState::INITIAL;
    struct WillStruct{
        std::string topic;
        std::vector<uint8_t> msg;
        QoS qos;
        Retain retain;
    };
    std::optional<WillStruct> mWill;
    std::atomic<uint16_t> mKeepAliveIntervalSeconds = 10;
    PersistentClientState* mPersistentState = nullptr;

    std::atomic<bool> mShouldBeDisconnected = false;
    std::string mClientId;
};

}