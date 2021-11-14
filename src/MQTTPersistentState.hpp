#pragma once

#include "Enums.hpp"
#include "Forward.hpp"
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <optional>

namespace nioev {

static inline bool operator==(const std::reference_wrapper<MQTTClientConnection>& a, const std::reference_wrapper<MQTTClientConnection>& b) {
    return &a.get() == &b.get();
}

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
};

}
