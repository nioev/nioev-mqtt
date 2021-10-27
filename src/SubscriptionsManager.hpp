#pragma once

#include "Enums.hpp"
#include "Forward.hpp"
#include <functional>
#include <shared_mutex>
#include <string>

namespace nioev {

class SubscriptionsManager {
public:
    void addSubscription(MQTTClientConnection& conn, std::string topic, QoS qos);
    void deleteSubscription(MQTTClientConnection& conn, const std::string& topic);
    void deleteAllSubscriptions(MQTTClientConnection& conn);

    struct Subscription {
        std::reference_wrapper<MQTTClientConnection> conn;
        const std::string topic;
        const QoS qos;
        Subscription(MQTTClientConnection& conn, std::string topic, QoS qos)
        : conn(conn), topic(std::move(topic)), qos(qos) {

        }
    };
    void forEachSubscriber(const std::string& topic, std::function<void(Subscription&)> callback);
private:
    std::unordered_multimap<std::string, Subscription> mSimpleSubscriptions;
    // TODO wildcard subscriptions
    std::shared_mutex mMutex;
};

}
