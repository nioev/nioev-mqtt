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
#include "Forward.hpp"

namespace nioev {

class MQTTClientConnection final : public Subscriber {
public:
    MQTTClientConnection(ApplicationState& app, TcpClientConnection&& conn)
    : mConn(std::move(conn)), mApp(app) {
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
    enum class SendDataType {
        DEFAULT,
        PUBLISH
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
    };
    std::pair<std::reference_wrapper<PacketReceiveData>, std::unique_lock<std::mutex>> getRecvData() {
        std::unique_lock<std::mutex> lock{mRecvMutex};
        return {mRecvData, std::move(lock)};
    }

    struct SendTask {
        util::SharedBuffer bytes;
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
    // TODO Remove mutex - I'm pretty sure we don't actually need it anymore, but it doesn't really matter and I'm not 100% sure
    std::pair<PersistentClientState*&, std::unique_lock<std::mutex>> getPersistentState() {
        std::unique_lock lock{mRemaingingMutex};
        return {mPersistentState, std::move(lock)};
    }
    void notifyLoggedOut() {
        mLoggedOut = true;
    }
    [[nodiscard]] bool isLoggedOut() const {
        return mLoggedOut;
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
        return std::dynamic_pointer_cast<MQTTClientConnection>(shared_from_this());
    }
    int64_t getLastDataRecvTimestamp() const {
        return mLastDataReceivedTimestamp;
    }
    void setLastDataRecvTimestamp(int64_t newTimestamp) {
        mLastDataReceivedTimestamp = newTimestamp;
    }

    bool hasSendError() {
        return mSendError;
    }

    void sendData(util::SharedBuffer&& bytes, SendDataType type = SendDataType::DEFAULT);
    void publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained, MQTTPublishPacketBuilder& packetBuilder) override;

    virtual const char* getType() const override {
        return "mqtt client";
    }
private:
    ApplicationState& mApp;
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

    std::atomic<bool> mLoggedOut = false, mSendError = false;
    std::string mClientId;
    std::atomic<int64_t> mLastDataReceivedTimestamp = std::chrono::steady_clock::now().time_since_epoch().count();
};

}