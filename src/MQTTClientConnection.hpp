#pragma once

#include <string>
#include <cstdint>
#include <thread>
#include <mutex>
#include <optional>

#include "Forward.hpp"
#include "TcpClientConnection.hpp"
#include "Enums.hpp"

namespace nioev {

class MQTTClientConnection {
public:
    MQTTClientConnection(TcpClientConnection&& conn)
    : mConn(std::move(conn)) {

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
    std::pair<std::reference_wrapper<std::vector<SendTask>>, std::unique_lock<std::mutex>> getSendTasks() {
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
    std::pair<PersistentClientState*, std::unique_lock<std::mutex>> getPersistentState() {
        std::unique_lock lock{mRemaingingMutex};
        return {mPersistentState, std::move(lock)};
    }
    std::string getClientId();
    void notifyConnecionError() {
        mConn.close();
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
private:
    TcpClientConnection mConn;

    std::mutex mRecvMutex;
    PacketReceiveData mRecvData;

    std::mutex mSendMutex;
    std::vector<SendTask> mSendTasks;

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
};

}