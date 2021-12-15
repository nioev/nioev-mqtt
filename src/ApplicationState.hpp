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

namespace nioev {

static inline bool operator==(const std::reference_wrapper<MQTTClientConnection>& a, const std::reference_wrapper<MQTTClientConnection>& b) {
    return &a.get() == &b.get();
}

enum class SessionPresent {
    No,
    Yes
};

struct ChangeRequestSubscribe {
    std::shared_ptr<Subscriber> subscriber;
    std::string topic;
    std::vector<std::string> topicSplit; // only relevant for wildcard subscriptions
    bool isWildcardSub = false;
    QoS qos;
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


struct ChangeRequestDeleteDisconnectedClients {

};

struct ChangeRequestDisconnectClient {
    std::shared_ptr<MQTTClientConnection> client;
};

using ChangeRequest = std::variant<ChangeRequestSubscribe, ChangeRequestUnsubscribe, ChangeRequestRetain, ChangeRequestDeleteDisconnectedClients, ChangeRequestDisconnectClient, ChangeRequestLoginClient>;

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
    void operator()(ChangeRequestDeleteDisconnectedClients&& req);
    void operator()(ChangeRequestLoginClient&& req);
    void operator()(ChangeRequestDisconnectClient&& req);

    void publish(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain);
    void handleNewClientConnection(TcpClientConnection&&) override;
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

    void publishWithoutAcquiringMutex(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain);
    void executeChangeRequest(ChangeRequest&&);
    void workerThreadFunc();

    void cleanupDisconnectedClients();

    void logoutClient(MQTTClientConnection& client, std::unique_lock<std::shared_mutex>& lock);
    // ensure you have at least a readonly lock when calling
    template<typename T = Subscriber>
    void forEachSubscriber(const std::string& topic, std::function<void(Subscription&)>&& callback) {
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

    std::shared_mutex mClientsMutex;
    std::list<std::shared_ptr<MQTTClientConnection>> mClients;

    std::shared_mutex mMutex;
    std::condition_variable mQueueCV;
    std::queue<ChangeRequest> mQueueInternal;
    atomic_queue::AtomicQueue2<ChangeRequest, 1024> mQueue;
    std::unordered_multimap<std::string, Subscription> mSimpleSubscriptions;
    std::vector<Subscription> mWildcardSubscriptions;
    struct RetainedMessage {
        std::vector<uint8_t> payload;
    };
    std::unordered_map<std::string, RetainedMessage> mRetainedMessages;
    std::unordered_map<std::string, PersistentClientState> mPersistentClientStates;

    std::atomic<bool> mShouldRun = true;
    std::thread mWorkerThread;

    Timers mTimers;

    // needs to initialized last because it starts a thread which calls us
    ClientThreadManager mClientManager;

};

}