#pragma once

#include <string>
#include <cstdint>
#include <thread>
#include <mutex>
#include <optional>
#include <queue>

#include "Forward.hpp"
#include "MQTTPublishPacketBuilder.hpp"
#include "nioev/lib/Enums.hpp"
#include "Subscriber.hpp"
#include "TcpClientConnection.hpp"

namespace nioev::mqtt {

class MQTTClientConnection : public TaskQueueRefCount {
public:
    MQTTClientConnection(ApplicationState& app, TcpClientConnection&& conn)
    : mConn(std::move(conn)), mApp(app) {
    }

    [[nodiscard]] TcpClientConnection& getTcpClient() {
        return mConn;
    }
    [[nodiscard]] const TcpClientConnection& getTcpClient() const {
        return mConn;
    }

    enum class ConnectionState {
        INITIAL,
        CONNECTING,
        CONNECTED,
        INVALID_PROTOCOL_VERSION
    };
    [[nodiscard]] ConnectionState getState(std::unique_lock<std::mutex>& recvMutex) const {
        assert(recvMutex.owns_lock());
        assert(recvMutex.mutex() == &mRecvMutex);
        return mState;
    }
    ConnectionState getStateAtomic() const {
        return mState;
    }
    void setStateAtomic(ConnectionState newState) {
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
    PacketReceiveData& getRecvData(std::unique_lock<std::mutex>& recvMutex) {
        assert(recvMutex.owns_lock());
        assert(recvMutex.mutex() == &mRecvMutex);
        return mRecvData;
    }

    std::pair<std::reference_wrapper<std::vector<InTransitEncodedPacket>>, std::unique_lock<std::timed_mutex>> getSendTasks() {
        std::unique_lock<std::timed_mutex> lock{mSendMutex};
        return {mSendTasks, std::move(lock)};
    }

    void setWill(std::unique_lock<std::mutex>& recvMutex, std::string&& topic, std::vector<uint8_t>&& msg, QoS qos, Retain retain) {
        assert(recvMutex.owns_lock());
        assert(recvMutex.mutex() == &mRecvMutex);
        mWill.emplace();
        mWill->topic = std::move(topic);
        mWill->payload = std::move(msg);
        mWill->qos = qos;
        mWill->retain = retain;
    }
    void discardWill(std::unique_lock<std::mutex>& recvMutex) {
        assert(recvMutex.owns_lock());
        assert(recvMutex.mutex() == &mRecvMutex);
        mWill.reset();
    }
    auto moveWill_NO_LOCK() {
        return std::move(mWill);
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
        assert(!mProperClientIdSet);
        mProperClientId = std::move(clientId);
        mProperClientIdSet = true;
    }
    const std::string& getClientId() {
        if(mProperClientIdSet)
            return mProperClientId;
        static std::string tmp{"< INVALID CLIENT ID >"};
        return tmp;
    }
    int64_t getLastDataRecvTimestamp() const {
        return mLastDataReceivedTimestamp;
    }
    void setLastDataRecvTimestamp(int64_t newTimestamp) {
        mLastDataReceivedTimestamp = newTimestamp;
    }

    void setConnectProperties(PropertyList properties) {
        mConnectProperties = std::move(properties);
    }
    const PropertyList& getConnectPropertyList() const {
        return mConnectProperties;
    }
    void setMQTTVersion(MQTTVersion version) {
        mMQTTVersion = version;
    }
    MQTTVersion getMQTTVersion() const {
        return mMQTTVersion;
    }
    bool hasSendError() {
        return mSendError;
    }
    PersistentClientState* getPersistentClientState() {
        return mPersistentClientState;
    }
    void setPersistentClientState(PersistentClientState* state) {
        mPersistentClientState = state;
    }
    void pushPacketReceivedWhileConnecting(std::unique_lock<std::mutex>& recvMutex, const PacketReceiveData& packet) {
        assert(recvMutex.owns_lock());
        assert(recvMutex.mutex() == &mRecvMutex);
        mPacketsReceivedWhileWaitingForConnectingLogin.emplace_back(packet);
    }
    std::vector<PacketReceiveData>& getPacketsReceivedWhileConnecting(std::unique_lock<std::mutex>& recvMutex) {
        assert(recvMutex.owns_lock());
        assert(recvMutex.mutex() == &mRecvMutex);
        return mPacketsReceivedWhileWaitingForConnectingLogin;
    }
    std::unique_lock<std::mutex> getRecvMutexLock() {
        return std::unique_lock<std::mutex>{ mRecvMutex };
    }

    void sendData(EncodedPacket packet);
    void sendData(InTransitEncodedPacket packet);
    void publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained, const PropertyList& properties, MQTTPublishPacketBuilder& packetBuilder, uint16_t packetId);

private:
    ApplicationState& mApp;
    TcpClientConnection mConn;

    std::atomic<PersistentClientState*> mPersistentClientState{nullptr};

    std::mutex mRecvMutex;
    PacketReceiveData mRecvData;
    std::vector<PacketReceiveData> mPacketsReceivedWhileWaitingForConnectingLogin;
    uint16_t mKeepAliveIntervalSeconds = 10;
    PropertyList mConnectProperties;
    MQTTVersion mMQTTVersion = MQTTVersion::V4;
    std::string mProperClientId;
    std::atomic<ConnectionState> mState = ConnectionState::INITIAL;
    std::optional<MQTTPacket> mWill;

    std::timed_mutex mSendMutex;
    std::vector<InTransitEncodedPacket> mSendTasks;



    std::atomic<bool> mLoggedOut = false, mSendError = false;
    std::atomic<int64_t> mLastDataReceivedTimestamp = std::chrono::steady_clock::now().time_since_epoch().count();

    std::atomic<bool> mProperClientIdSet{false};
};

}