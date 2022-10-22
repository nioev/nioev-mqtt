#pragma once

#include "AsyncPublisher.hpp"
#include "ClientThreadManager.hpp"
#include "MQTTClientConnection.hpp"
#include "MQTTPublishPacketBuilder.hpp"
#include "scripting/NativeLibraryCompiler.hpp"
#include "scripting/ScriptContainer.hpp"
#include "SQLiteCpp/Database.h"
#include "Statistics.hpp"
#include "TcpClientHandlerInterface.hpp"
#include "Timers.hpp"
#include "nioev/lib/SubscriptionTree.hpp"
#include <atomic_queue/atomic_queue.h>
#include <bitset>
#include <condition_variable>
#include <list>
#include <memory>
#include <shared_mutex>
#include <unordered_set>
#include <variant>
#include <vector>

namespace nioev::mqtt {
struct Subscription {
    Subscriber* subscriber = nullptr;
    QoS qos = QoS::QoS0;
    Subscription() {

    }
    Subscription(Subscriber* conn, QoS qos)
    : subscriber(std::move(conn)), qos(qos) {

    }
    bool operator==(const Subscription& b) const {
        return subscriber == b.subscriber;
    }
};
}

namespace std {
template<>
struct hash<nioev::mqtt::Subscription> {
    size_t operator()(const nioev::mqtt::Subscription& sub) const {
        return std::hash<void*>()(sub.subscriber);
    }
};
}

namespace nioev::mqtt {

static inline bool operator==(const std::reference_wrapper<MQTTClientConnection>& a, const std::reference_wrapper<MQTTClientConnection>& b) {
    return &a.get() == &b.get();
}

enum class SessionPresent {
    No,
    Yes
};

enum class SubscriptionType {
    NORMAL,
    OMNI // receives ALL messages, even ones to $ topics
};

struct HighQoSRetainStorage {
public:
    // FIXME reencode data when connecting with different MQTT version (this is really weird though)
    HighQoSRetainStorage(EncodedPacket packet, QoS qos, MQTTVersion version)
    : mPacket(std::move(packet)), mQoS(qos), mMQTTVersion(version) {
        static std::atomic<uint64_t> globalOrderCounter{0};
        mGlobalOrderCount = globalOrderCounter++;
    }
    EncodedPacket getPacketSharedCopy() {
        return mPacket;
    }
    uint64_t getGlobalOrder() const {
        return mGlobalOrderCount;
    }
private:
    EncodedPacket mPacket;
    uint64_t mGlobalOrderCount{0};
    QoS mQoS;
    MQTTVersion mMQTTVersion;
};

class PersistentClientState : public Subscriber {
public:
    PersistentClientState(std::string clientId, CleanSession cleanSession, MQTTClientConnection* client)
    : mClientID(std::move(clientId)), mCleanSession(cleanSession), mCurrentClient(std::move(client)) {

    }
    void publish(const std::string& topic, PayloadType payload, QoS qos, Retained retained, const PropertyList& properties, MQTTPublishPacketBuilder& packetBuilder) override;
    virtual const char* getType() const override {
        return "mqtt client";
    }
    std::pair<MQTTClientConnection*, std::unique_lock<std::recursive_mutex>> getCurrentClient() {
        std::unique_lock<std::recursive_mutex> lock{mMutex};
        return std::make_pair(mCurrentClient, std::move(lock));
    }
    std::pair<const MQTTClientConnection*, std::unique_lock<std::recursive_mutex>> getCurrentClient() const {
        std::unique_lock<std::recursive_mutex> lock{mMutex};
        return std::make_pair(mCurrentClient, std::move(lock));
    }
    const std::string& getClientID() const {
        return mClientID;
    }
    CleanSession isCleanSession() const {
        return mCleanSession;
    }
    enum class ReplaceStyle {
        SimpleReplace,
        CleanSession
    };
    void replaceCurrentClient(std::unique_lock<std::recursive_mutex>& lock, MQTTClientConnection* newConnection, CleanSession cleanSession, ReplaceStyle replaceStyle) {
        assert(lock.owns_lock());
        if(mCurrentClient) {
            mCurrentClient->setPersistentClientState(nullptr);
        }
        mCurrentClient = newConnection;
        mCleanSession = cleanSession;
        mCurrentClient->setPersistentClientState(this);
        mCurrentClient->setClientId(mClientID);

        if(replaceStyle == ReplaceStyle::CleanSession) {
            mQos2pubrecReceived.reset();
            mQoS2receivingPacketIds.reset();
            mHighQoSSendingPackets.clear();
            mPacketIdCounter = 1;
        }
    }
    void dropCurrentClient() {
        std::unique_lock<std::recursive_mutex> lock{mMutex};
        mCurrentClient->setPersistentClientState(nullptr);
        mCurrentClient = nullptr;
    }
    std::unordered_map<uint16_t, HighQoSRetainStorage>& getHighQoSSendingPackets() {
        return mHighQoSSendingPackets;
    }
    std::bitset<256 * 256>& getQoS2PubRecReceived() {
        return mQos2pubrecReceived;
    }
    std::bitset<256 * 256>& getQoS2ReceivingPacketIds() {
        return mQoS2receivingPacketIds;
    }
    std::unique_lock<std::recursive_mutex> getLock() {
        return std::unique_lock<std::recursive_mutex>{mMutex};
    }

private:
    static_assert(std::is_same_v<decltype(std::chrono::steady_clock::time_point{}.time_since_epoch().count()), int64_t>);

    mutable std::recursive_mutex mMutex;

    std::unordered_map<uint16_t, HighQoSRetainStorage> mHighQoSSendingPackets;
    std::bitset<256 * 256> mQos2pubrecReceived;
    std::bitset<256 * 256> mQoS2receivingPacketIds;

    std::string mClientID;
    CleanSession mCleanSession = CleanSession::Yes;
    uint16_t mPacketIdCounter = 1;
    MQTTClientConnection* mCurrentClient = nullptr;
};

struct ChangeRequestSubscribe {
    Subscriber* subscriber;
    std::string topic;
    QoS qos = QoS::QoS0;
    SubscriptionType type = SubscriptionType::NORMAL;
};

struct ChangeRequestUnsubscribe {
    Subscriber* subscriber;
    std::string topic;
};
struct ChangeRequestUnsubscribeFromAll {
    Subscriber* subscriber;
};

struct ChangeRequestRetain {
    MQTTPacket packet;
};
struct ChangeRequestLoginClient {
    MQTTClientConnection* client;
    std::string clientId;
    CleanSession cleanSession;
};
struct ChangeRequestLogoutClient {
    MQTTClientConnection* client;
};

struct ChangeRequestAddScript {
    std::string name;
    std::string code;
    ScriptStatusOutput statusOutput;
    bool fromStartup{false};
};

struct ChangeRequestDeleteScript {
    std::string name;
};

struct ChangeRequestActivateScript {
    std::string name;
};

struct ChangeRequestDeactivateScript {
    std::string name;
};

using ChangeRequest = std::variant<ChangeRequestSubscribe, ChangeRequestUnsubscribe, ChangeRequestRetain, ChangeRequestLoginClient, ChangeRequestAddScript,
                                   ChangeRequestLogoutClient, ChangeRequestUnsubscribeFromAll, ChangeRequestDeleteScript,
                                   ChangeRequestActivateScript, ChangeRequestDeactivateScript>;


template<typename T>
class UniqueLockWithAtomicTidUpdate : public std::unique_lock<T> {
    std::atomic<std::thread::id>& mTid;
public:
    UniqueLockWithAtomicTidUpdate(T& lock, std::atomic<std::thread::id>& tid)
    : std::unique_lock<T>(lock), mTid(tid) {
        mTid = std::this_thread::get_id();
    }
    ~UniqueLockWithAtomicTidUpdate() {
        mTid = std::thread::id();
    }
};

class ApplicationState : public TcpClientHandlerInterface {
public:
    ApplicationState();
    ~ApplicationState();
    enum class RequestChangeMode {
        ASYNC,
        SYNC,
        TRY_SYNC_THEN_ASYNC
    };
    void requestChange(ChangeRequest&&, RequestChangeMode = RequestChangeMode::ASYNC);

    // DON'T CALL THESE! They exist only for std::visit to work!
    void operator()(ChangeRequestSubscribe&& req);
    void operator()(ChangeRequestUnsubscribe&& req);
    void operator()(ChangeRequestUnsubscribeFromAll&& req);
    void operator()(ChangeRequestRetain&& req);
    void operator()(ChangeRequestLoginClient&& req);
    void operator()(ChangeRequestLogoutClient&& req);
    void operator()(ChangeRequestAddScript&& req);
    void operator()(ChangeRequestDeleteScript&& req);
    void operator()(ChangeRequestActivateScript&& req);
    void operator()(ChangeRequestDeactivateScript&& req);

    void publish(std::string&& topic, PayloadType msg, QoS qos, Retain retain, const PropertyList& properties);
    // The one-stop solution for all your async publishing needs! Need to publish something but you are actually called by publish itself, which
    // would cause deadlocks or stack overflows? Don't worry! Just call publishAsync and be certain that another thread will handle this problem for you!
    // This will probably even increase performance in case there are many subscribers and you are really busy yourself, because this will free up processing
    // time for you.
    // Serious note: Publishing doesn't change the application state, so there is no need to cram it into a ChangeRequest (which was actually how it
    // previously worked). This architecture caused problems because it meant that you had be really cautious when you log, because logging something
    // needs to publish data, which meant calling requestChange, but you aren't allowed to call requestChange while holding mMutex read-only, because
    // then we can't add stuff to mQueue, because that could cause deadlocks etc. This new architecture allows you to log at anytime anywhere (except
    // in publishAsync itself), though be aware that infinite loops are still possible if every logged message causes another message to be logged, which
    // causes another message to be logged etc. etc.
    void publishAsync(MQTTPacket&& data) {
        (void)mAsyncPublisher.enqueue(std::move(data));
    }

    void handleNewClientConnection(TcpClientConnection&&) override;

    struct ScriptsInfo {
        struct ScriptInfo {
            std::string name;
            std::string code;
            ScriptState active{ScriptState::UNKNOWN};
        };
        std::vector<ScriptInfo> scripts;
    };
    ScriptsInfo getScriptsInfo();

    void addScript(std::string name, std::function<void(const std::string& scriptName, const std::string& value)>&& onSuccess, std::function<void(const std::string& scriptName, const std::string&)>&& onError, std::string code);

    void syncRetainedMessagesToDb();

    void runScript(const std::string& name, const ScriptInputArgs& input, ScriptStatusOutput&& output);

    auto getListOfCurrentlyLoadingNativeLibs() const {
        return mNativeLibManager.getListOfCurrentlyLoadingNativeLibs();
    }

    WorkerThreadSleepLevel getCurrentWorkerThreadSleepLevel() const {
        return mWorkerThreadSleepLevel;
    }
    unsigned getCurrentWorkerThreadQueueDepth() const {
        return mQueue.was_size();
    }
    AnalysisResults getAnalysisResults() {
        return mStatistics->getResults();
    }
    uint64_t getRetainedMsgCount() {
        std::shared_lock<std::shared_mutex> lock{mMutex};
        return mRetainedMessages.size();
    }
    uint64_t getRetainedMsgCummulativeSize() {
        std::shared_lock<std::shared_mutex> lock{mMutex};
        uint64_t sum = 0;
        for(auto& msg: mRetainedMessages) {
            sum += msg.second.payload.size() + msg.first.size() + 1;
        }
        return sum;
    }
    std::unordered_map<std::string, uint64_t> getSubscriptionsCount();
    template<typename T> void forEachClient(T&& callback) const {
        std::shared_lock<std::shared_mutex> lock{mMutex};
        for(auto& c: mPersistentClientStates) {
            auto [client, clientLock] = c.second->getCurrentClient();
            if(client) {
                callback(c.second->getClientID(), client->getTcpClient().getRemoteIp(), client->getTcpClient().getRemotePort());
            } else {
                callback(c.second->getClientID());
            }
        }
    }
private:

    // basically the same as publish but without acquiring a read-only lock
    void publishInternal(std::string&& topic, PayloadType msg, QoS qos, Retain retain, const PropertyList& properties);

    Retain publishNoLockNoRetain(const std::string& topic, PayloadType msg, QoS publishQoS, Retain retain, const PropertyList& properties);
    void executeChangeRequest(ChangeRequest&&);
    void workerThreadFunc();

    void cleanup();

    enum class ShouldPersistSubscription {
        Yes,
        No
    };
    void subscribeClientInternal(ChangeRequestSubscribe&& req, ShouldPersistSubscription);

    void logoutClient(MQTTClientConnection& client);

    void deleteScript(std::unordered_map<std::string, std::unique_ptr<ScriptContainer>>::iterator it);
    void deleteAllSubscriptions(Subscriber& sub);

    void performSystemAction(const std::string& topic, std::string_view payloadStr);


    // the order of these fields has been carefully evaluated and tested to be race-free during deconstruction, so be careful to change anything!

    mutable std::shared_mutex mMutex;
    std::atomic<std::thread::id> mCurrentRWHolderOfMMutex;

    NativeLibraryCompiler mNativeLibManager;

    SubscriptionTree<Subscription> mSubscriptions;

    std::list<ChangeRequest> mQueueInternal;
    atomic_queue::AtomicQueue2<ChangeRequest, 4096> mQueue;
    std::atomic<bool> mShouldCleanup = false;

    AsyncPublisher mAsyncPublisher;

    std::list<MQTTClientConnection> mClients;
    std::unordered_map<std::string, std::unique_ptr<PersistentClientState>> mPersistentClientStates;
    std::vector<std::unique_ptr<PersistentClientState>> mDeletedPersistentClientStates;

    struct RetainedMessage {
        std::vector<uint8_t> payload;
        std::time_t timestamp;
        QoS qos{QoS::QoS0};
        PropertyList properties;
    };
    std::unordered_map<std::string, RetainedMessage> mRetainedMessages;

    std::atomic<bool> mShouldRun = true;
    std::atomic<WorkerThreadSleepLevel> mWorkerThreadSleepLevel;

    size_t mAmountOfScriptsLoadedFromDBOnStartup{0};
    std::atomic<size_t> mScriptsAlreadyAddedFromStartup{0};

    Timers mTimers;
    std::unordered_map<std::string, std::unique_ptr<ScriptContainer>> mScripts;
    std::shared_ptr<Statistics> mStatistics;

    SQLite::Database mDb{"nioev.db3", SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE};
    std::optional<SQLite::Statement> mQueryInsertScript;
    std::optional<SQLite::Statement> mQueryInsertRetainedMsg;

    // needs to initialized last because it starts a thread which calls us
    ClientThreadManager mClientManager;
    std::thread mWorkerThread;

};

}