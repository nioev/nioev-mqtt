#include "SubscriptionsManager.hpp"
#include "MQTTClientConnection.hpp"

namespace nioev {

void SubscriptionsManager::addSubscription(MQTTClientConnection& conn, std::string topic, QoS qos) {
    std::lock_guard<std::shared_mutex> lock{mMutex};
    mSimpleSubscriptions.emplace(std::piecewise_construct,
                                 std::make_tuple(topic),
                                 std::make_tuple(std::reference_wrapper<MQTTClientConnection>(conn), topic, qos));
}
void SubscriptionsManager::deleteSubscription(MQTTClientConnection& conn, const std::string& topic) {
    std::lock_guard<std::shared_mutex> lock{mMutex};
    auto[start, end] = mSimpleSubscriptions.equal_range(topic);
    for(auto it = start; it != end;) {
        if(&it->second.conn.get() == &conn) {
            it = mSimpleSubscriptions.erase(it);
        } else {
            it++;
        }
    }
}
void SubscriptionsManager::deleteAllSubscriptions(MQTTClientConnection& conn) {
    std::lock_guard<std::shared_mutex> lock{mMutex};
    for(auto it = mSimpleSubscriptions.begin(); it != mSimpleSubscriptions.end();) {
        if(&it->second.conn.get() == &conn) {
            it = mSimpleSubscriptions.erase(it);
        } else {
            it++;
        }
    }
}
void SubscriptionsManager::forEachSubscriber(const std::string& topic, std::function<void(Subscription&)> callback) {
    std::shared_lock<std::shared_mutex> lock{mMutex};
    auto[start, end] = mSimpleSubscriptions.equal_range(topic);
    for(auto it = start; it != end; ++it) {
        callback(it->second);
    }
}

}
