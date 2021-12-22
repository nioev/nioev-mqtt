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
#include "scripting/ScriptContainer.hpp"

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

struct ChangeRequestRetain {
    std::string topic;
    std::vector<uint8_t> payload;
};
struct ChangeRequestLoginClient {
    std::shared_ptr<MQTTClientConnection> client;
    std::string clientId;
    CleanSession cleanSession;
};

struct ChangeRequestDisconnectClient {
    std::shared_ptr<MQTTClientConnection> client;
};

struct ChangeRequestAddScript {
    std::string name;
    std::function<std::shared_ptr<ScriptContainer>()> constructor;
    ScriptStatusOutput statusOutput;
};

struct ChangeRequestPublish {
    std::string topic;
    std::vector<uint8_t> payload;
    QoS qos;
    Retain retain;
};

using ChangeRequest = std::variant<ChangeRequestSubscribe, ChangeRequestUnsubscribe, ChangeRequestRetain, ChangeRequestDisconnectClient, ChangeRequestLoginClient, ChangeRequestAddScript, ChangeRequestPublish>;

struct PersistentClientState {
    static_assert(std::is_same_v<decltype(std::chrono::steady_clock::time_point{}.time_since_epoch().count()), int64_t>);

    MQTTClientConnection *currentClient = nullptr;
    int64_t lastDisconnectTime = 0;
    std::unordered_set<uint16_t> qos3receivingPacketIds;
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
        tid = std::this_thread::get_id();
    }
    ~UniqueLockWithAtomicTidUpdate() {

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
    void operator()(ChangeRequestRetain&& req);
    void operator()(ChangeRequestLoginClient&& req);
    void operator()(ChangeRequestDisconnectClient&& req);
    void operator()(ChangeRequestAddScript&& req);
    void operator()(ChangeRequestPublish&& req);

    void publish(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain);
    void handleNewClientConnection(TcpClientConnection&&) override;

    struct ScriptsInfo {
        struct ScriptInfo {
            std::string name;
            std::string code;
        };
        std::vector<ScriptInfo> scripts;
    };
    ScriptsInfo getScriptsInfo();

    template<typename T, typename... Args>
    void addScript(const std::string& name, std::function<void(const std::string& scriptName)>&& onSuccess, std::function<void(const std::string& scriptName, const std::string&)>&& onError, Args&&... args) {
        auto lambda = [this, name, args = std::move(args...)] () mutable -> std::shared_ptr<ScriptContainer> {
            return std::dynamic_pointer_cast<ScriptContainer>(std::make_shared<T>(*this, name, std::forward<Args>(args)...));
        };
        ScriptStatusOutput statusOutput;
        statusOutput.success = std::move(onSuccess);
        auto originalError = std::move(statusOutput.error);
        statusOutput.error = [onError = std::move(onError), originalError = std::move(originalError)] (auto& scriptName, const auto& error) {
            originalError(scriptName, error);
            onError(scriptName, error);
        };
        requestChange(ChangeRequestAddScript{std::move(name), std::move(lambda), std::move(statusOutput)});
    }
private:
    struct Subscription {
        std::shared_ptr<Subscriber> subscriber;
        std::string topic;
        std::vector<std::string> topicSplit; // only used for wildcard subscriptions
        std::optional<QoS> qos;
        Subscription(std::shared_ptr<Subscriber> conn, std::string topic, std::vector<std::string>&& topicSplit, std::optional<QoS> qos)
        : subscriber(std::move(conn)), topic(std::move(topic)), topicSplit(std::move(topicSplit)), qos(qos) {

        }
    };

    // basically the same as publish but without acquiring a read-only lock
    void publishInternal(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain);

    void publishNoLockNoRetain(const std::string& topic, const std::vector<uint8_t>& msg, std::optional<QoS> qos, Retain retain);
    void executeChangeRequest(ChangeRequest&&);
    void workerThreadFunc();

    void cleanup();

    void logoutClient(MQTTClientConnection& client);
    // ensure you have at least a readonly lock when calling
    template<typename T = Subscriber>
    void forEachSubscriberThatIsOfT(const std::string& topic, std::function<void(Subscription&)>&& callback) {
        for(auto& sub: mOmniSubscriptions) {
            if(std::dynamic_pointer_cast<T>(sub.subscriber) == nullptr)
                continue;
            callback(sub);
        }
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
    }
    template<typename T = Subscriber>
    void forEachSubscriberThatIsNotOfT(const std::string& topic, std::function<void(Subscription&)>&& callback) {
        for(auto& sub: mOmniSubscriptions) {
            if(std::dynamic_pointer_cast<T>(sub.subscriber) != nullptr)
                continue;
            callback(sub);
        }
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
    }
    void deleteScript(std::unordered_map<std::string, std::shared_ptr<ScriptContainer>>::iterator it);
    void deleteAllSubscriptions(Subscriber& sub);

    std::list<std::shared_ptr<MQTTClientConnection>> mClients;

    std::shared_mutex mMutex;
    std::atomic<std::thread::id> mCurrentRWHolderOfMMutex;

    std::queue<ChangeRequest> mQueueInternal;
    atomic_queue::AtomicQueue2<ChangeRequest, 2048> mQueue;
    std::unordered_multimap<std::string, Subscription> mSimpleSubscriptions;
    std::vector<Subscription> mWildcardSubscriptions;
    std::vector<Subscription> mOmniSubscriptions;

    struct RetainedMessage {
        std::vector<uint8_t> payload;
    };
    std::unordered_map<std::string, RetainedMessage> mRetainedMessages;
    std::unordered_map<std::string, PersistentClientState> mPersistentClientStates;

    std::atomic<bool> mShouldRun = true;
    std::thread mWorkerThread;

    Timers mTimers;
    std::unordered_map<std::string, std::shared_ptr<ScriptContainer>> mScripts;

    // needs to initialized last because it starts a thread which calls us
    ClientThreadManager mClientManager;

};

}