#pragma once

#include "MQTTClientConnection.hpp"
#include "ClientThreadManager.hpp"
#include <shared_mutex>
#include <list>
#include <memory>
#include <unordered_set>
#include <vector>
#include <atomic_queue/atomic_queue.h>
#include <variant>
#include <condition_variable>
#include "TcpClientHandlerInterface.hpp"
#include "Timers.hpp"
#include "AsyncPublisher.hpp"
#include "scripting/ScriptContainer.hpp"
#include "scripting/NativeLibraryCompiler.hpp"
#include "SQLiteCpp/Database.h"
#include "Statistics.hpp"
#include <bitset>

namespace nioev {

static inline bool operator==(const std::reference_wrapper<MQTTClientConnection>& a, const std::reference_wrapper<MQTTClientConnection>& b) {
    return &a.get() == &b.get();
}

enum class SessionPresent {
    No,
    Yes
};

enum class SubscriptionType {
    SIMPLE,
    WILDCARD,
    OMNI // receives ALL messages, even ones to $ topics
};

struct ChangeRequestSubscribe {
    std::shared_ptr<Subscriber> subscriber;
    std::string topic;
    std::vector<std::string> topicSplit; // only relevant for wildcard subscriptions
    SubscriptionType subType;
    QoS qos = QoS::QoS0;
};

struct ChangeRequestUnsubscribe {
    std::shared_ptr<Subscriber> subscriber;
    std::string topic;
};
struct ChangeRequestUnsubscribeFromAll {
    std::shared_ptr<Subscriber> subscriber;
};

struct ChangeRequestRetain {
    std::string topic;
    std::vector<uint8_t> payload;
    QoS qos{QoS::QoS0};
};
struct ChangeRequestLoginClient {
    std::shared_ptr<MQTTClientConnection> client;
    std::string clientId;
    CleanSession cleanSession;
};
struct ChangeRequestLogoutClient {
    std::shared_ptr<MQTTClientConnection> client;
};

struct ChangeRequestAddScript {
    std::string name;
    std::string code;
    ScriptStatusOutput statusOutput;
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

static inline ChangeRequest makeChangeRequestSubscribe(std::shared_ptr<Subscriber> sub, std::string&& topic, QoS qos = QoS::QoS0, bool isOmni = false) {
    assert(isOmni == false); // they are currently not used, so if you need an omni subscription, test thorougly!
    auto split = util::splitTopics(topic);
    SubscriptionType type;
    if(isOmni) {
        type = SubscriptionType::OMNI;
    } else {
        type = util::hasWildcard(topic) ? SubscriptionType::WILDCARD : SubscriptionType::SIMPLE;
    }
    return ChangeRequestSubscribe{
        std::move(sub),
        std::move(topic),
        std::move(split),
        type,
        qos
    };
}

// almost everything here is protected by the ApplicationState::mMutex lock
struct PersistentClientState {
    static_assert(std::is_same_v<decltype(std::chrono::steady_clock::time_point{}.time_since_epoch().count()), int64_t>);

    MQTTClientConnection *currentClient = nullptr;
    int64_t lastDisconnectTime = 0;

    // these two are protected by the lock in MQTTClientConnection::mRemainingMutex
    std::unordered_map<uint16_t, util::SharedBuffer> qos1sendingPackets;
    std::unordered_map<uint16_t, util::SharedBuffer> qos2sendingPackets;
    std::bitset<256 * 256> qos2pubrecReceived;
    std::bitset<256 * 256> qos2receivingPacketIds;

    std::string clientId;
    CleanSession cleanSession = CleanSession::Yes;
    struct PersistentSubscription {
        std::string topic;
        QoS qos;
    };
    std::vector<PersistentSubscription> subscriptions;
};

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
        SYNC_WHEN_IDLE
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

    void publish(std::string&& topic, std::vector<uint8_t>&& msg, QoS qos, Retain retain);
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
    void publishAsync(AsyncPublishData&& data) {
        mAsyncPublisher.enqueue(std::move(data));
    }

    void handleNewClientConnection(TcpClientConnection&&) override;

    struct ScriptsInfo {
        struct ScriptInfo {
            std::string name;
            std::string code;
            bool active{false};
        };
        std::vector<ScriptInfo> scripts;
    };
    ScriptsInfo getScriptsInfo();

    void addScript(std::string name, std::function<void(const std::string& scriptName)>&& onSuccess, std::function<void(const std::string& scriptName, const std::string&)>&& onError, std::string code);

    void syncRetainedMessagesToDb();

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
private:
    struct Subscription {
        std::shared_ptr<Subscriber> subscriber;
        std::string topic;
        std::vector<std::string> topicSplit; // only used for wildcard subscriptions
        QoS qos;
        Subscription(std::shared_ptr<Subscriber> conn, std::string topic, std::vector<std::string>&& topicSplit, QoS qos)
        : subscriber(std::move(conn)), topic(std::move(topic)), topicSplit(std::move(topicSplit)), qos(qos) {

        }
    };

    // basically the same as publish but without acquiring a read-only lock
    void publishInternal(std::string&& topic, std::vector<uint8_t>&& msg, QoS qos, Retain retain);

    void publishNoLockNoRetain(const std::string& topic, const std::vector<uint8_t>& msg, QoS publishQoS, Retain retain);
    void executeChangeRequest(ChangeRequest&&);
    void workerThreadFunc();

    void cleanup();

    enum class ShouldPersistSubscription {
        Yes,
        No
    };
    void subscribeClientInternal(ChangeRequestSubscribe&& req, ShouldPersistSubscription);

    void logoutClient(MQTTClientConnection& client);
    // ensure you have at least a readonly lock when calling
    template<typename T = Subscriber>
    void forEachSubscriberThatIsOfT(const std::string& topic, std::function<void(Subscription&)>&& callback) {
        auto[start, end] = mSimpleSubscriptions.equal_range(topic);
        for(auto it = start; it != end; ++it) {
            if(std::dynamic_pointer_cast<T>(it->second.subscriber) == nullptr)
                continue;
            callback(it->second);
        }
        for(auto& sub: mWildcardSubscriptions) {
            if(std::dynamic_pointer_cast<T>(sub.subscriber) == nullptr)
                continue;
            if(util::doesTopicMatchSubscription(topic, sub.topicSplit)) {
                callback(sub);
            }
        }
        for(auto& sub: mOmniSubscriptions) {
            if(std::dynamic_pointer_cast<T>(sub.subscriber) == nullptr)
                continue;
            callback(sub);
        }
    }
    template<typename T = Subscriber, typename Callback>
    void forEachSubscriberThatIsNotOfT(const std::string& topic, Callback&& callback) {
        auto[start, end] = mSimpleSubscriptions.equal_range(topic);
        for(auto it = start; it != end; ++it) {
            if(std::dynamic_pointer_cast<T>(it->second.subscriber) != nullptr)
                continue;
            callback(it->second);
        }
        for(auto& sub: mWildcardSubscriptions) {
            if(std::dynamic_pointer_cast<T>(sub.subscriber) != nullptr)
                continue;
            if(util::doesTopicMatchSubscription(topic, sub.topicSplit)) {
                callback(sub);
            }
        }
        for(auto& sub: mOmniSubscriptions) {
            if(std::dynamic_pointer_cast<T>(sub.subscriber) != nullptr)
                continue;
            callback(sub);
        }
    }
    void deleteScript(std::unordered_map<std::string, std::shared_ptr<ScriptContainer>>::iterator it);
    void deleteAllSubscriptions(Subscriber& sub);

    // the order of these fields has been carefully evaluated and tested to be race-free during deconstruction, so be careful to change anything!

    std::shared_mutex mMutex;
    std::atomic<std::thread::id> mCurrentRWHolderOfMMutex;

    NativeLibraryCompiler mNativeLibManager;

    std::unordered_multimap<std::string, Subscription> mSimpleSubscriptions;
    std::vector<Subscription> mWildcardSubscriptions;
    std::vector<Subscription> mOmniSubscriptions;

    std::queue<ChangeRequest> mQueueInternal;
    atomic_queue::AtomicQueue2<ChangeRequest, 4096> mQueue;
    bool mClientsWereLoggedOutSinceLastCleanup = false;

    AsyncPublisher mAsyncPublisher;

    std::list<std::shared_ptr<MQTTClientConnection>> mClients;

    struct RetainedMessage {
        std::vector<uint8_t> payload;
        std::time_t timestamp;
        QoS qos{QoS::QoS0};
    };
    std::unordered_map<std::string, RetainedMessage> mRetainedMessages;
    std::unordered_map<std::string, PersistentClientState> mPersistentClientStates;

    std::atomic<bool> mShouldRun = true;
    std::atomic<WorkerThreadSleepLevel> mWorkerThreadSleepLevel;

    Timers mTimers;
    std::unordered_map<std::string, std::shared_ptr<ScriptContainer>> mScripts;
    std::shared_ptr<Statistics> mStatistics;

    SQLite::Database mDb{"nioev.db3", SQLite::OPEN_READWRITE|SQLite::OPEN_CREATE};
    std::optional<SQLite::Statement> mQueryInsertScript;
    std::optional<SQLite::Statement> mQueryInsertRetainedMsg;

    // needs to initialized last because it starts a thread which calls us
    ClientThreadManager mClientManager;
    std::thread mWorkerThread;

};

}