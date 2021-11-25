#pragma once

#include "Enums.hpp"
#include "Forward.hpp"
#include <functional>
#include <shared_mutex>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <optional>
#include <unordered_set>
#include <atomic>

namespace nioev {

static inline bool operator==(const std::reference_wrapper<MQTTClientConnection>& a, const std::reference_wrapper<MQTTClientConnection>& b) {
    return &a.get() == &b.get();
}


struct PersistentClientState {
    std::atomic<MQTTClientConnection*> currentClient = nullptr;
    std::atomic<int64_t> lastDisconnectTime = 0;

    static_assert(std::is_same_v<decltype(std::chrono::steady_clock::time_point{}.time_since_epoch().count()), int64_t>);
    // when isConnected is true, requires the clients receive lock
    std::unordered_set<uint16_t> qos3receivingPacketIds;
    std::string clientId;
    CleanSession cleanSession = CleanSession::Yes;
};

enum class SessionPresent {
    No,
    Yes
};

class MQTTPersistentState {
public:
    using ScriptName = std::string;
    void addSubscription(MQTTClientConnection& conn, std::string topic, QoS qos, std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback);
    void addSubscription(std::string scriptName, std::string topic, std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback);
    void deleteSubscription(std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName> subscriber, const std::string& topic);
    void deleteAllSubscriptions(std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName> subscriber);

    struct Subscription {
        std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName> subscriber;
        std::string topic;
        std::vector<std::string> topicSplit; // only used for wildcard subscriptions
        std::optional<QoS> qos;
        Subscription(std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName> conn, std::string topic, std::vector<std::string>&& topicSplit, std::optional<QoS> qos)
        : subscriber(std::move(conn)), topic(std::move(topic)), topicSplit(std::move(topicSplit)), qos(qos) {

        }
    };

    void forEachSubscriber(const std::string& topic, std::function<void(Subscription&)> callback);
    void retainMessage(std::string&& topic, std::vector<uint8_t>&& payload);

    SessionPresent loginClient(MQTTClientConnection& conn, std::string&& clientId, CleanSession cleanSession);
    // Disassosciates the persistent client state and parameter
    void logoutClient(MQTTClientConnection& conn);
private:

    void addSubscriptionInternal(std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName> subscriber, std::string topic, std::optional<QoS> qos, std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback);

    std::shared_mutex mSubscriptionsMutex;
    std::unordered_multimap<std::string, Subscription> mSimpleSubscriptions;
    std::vector<Subscription> mWildcardSubscriptions;
    struct RetainedMessage {
        std::vector<uint8_t> payload;
    };
    std::shared_mutex mRetainedMessagesMutex;
    std::unordered_map<std::string, RetainedMessage> mRetainedMessages;


    std::recursive_mutex mPersistentClientStatesMutex;
    std::unordered_map<std::string, PersistentClientState> mPersistentClientStates;
};

}
