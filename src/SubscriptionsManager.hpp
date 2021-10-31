#pragma once

#include "Enums.hpp"
#include "Forward.hpp"
#include <functional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

namespace nioev {

class SubscriptionsManager {
public:
    void addSubscription(MQTTClientConnection& conn, std::string topic, QoS qos, std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback);
    void deleteSubscription(MQTTClientConnection& conn, const std::string& topic);
    void deleteAllSubscriptions(MQTTClientConnection& conn);

    struct Subscription {
        std::reference_wrapper<MQTTClientConnection> conn;
        std::string topic;
        std::vector<std::string> topicSplit; // only used for wildcard subscriptions
        QoS qos;
        Subscription(MQTTClientConnection& conn, std::string topic, std::vector<std::string>&& topicSplit, QoS qos)
        : conn(conn), topic(std::move(topic)), topicSplit(std::move(topicSplit)), qos(qos) {

        }
    };

    void forEachSubscriber(const std::string& topic, std::function<void(Subscription&)> callback);
    void retainMessage(std::string&& topic, std::vector<uint8_t>&& payload);
private:
    std::unordered_multimap<std::string, Subscription> mSimpleSubscriptions;
    std::vector<Subscription> mWildcardSubscriptions;
    std::shared_mutex mMutex;
    struct RetainedMessage {
        std::vector<uint8_t> payload;
    };
    std::unordered_map<std::string, RetainedMessage> mRetainedMessages;
};

}
