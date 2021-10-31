#include "SubscriptionsManager.hpp"
#include "MQTTClientConnection.hpp"

namespace nioev {

enum class IterationDecision {
    Continue,
    Stop
};

template<typename T>
static void splitString(const std::string& str, T callback) {
    std::string::size_type offset = 0, nextOffset = 0;
    do {
        nextOffset = str.find('/', offset);
        if(callback(std::string_view{str}.substr(offset, nextOffset - offset)) == IterationDecision::Stop) {
            break;
        }
        offset = nextOffset + 1;
    } while(nextOffset != std::string::npos);
}

void SubscriptionsManager::addSubscription(MQTTClientConnection& conn, std::string topic, QoS qos) {
    std::lock_guard<std::shared_mutex> lock{mMutex};
    auto hasWildcard = std::any_of(topic.begin(), topic.end(), [](char c) {
                           return c == '#' || c == '+';
                       });
    if(hasWildcard) {
        std::vector<std::string> parts;
        splitString(topic, [&parts](const std::string_view& part) {
            parts.emplace_back(part);
            return IterationDecision::Continue;
        });
        mWildcardSubscriptions.emplace_back(conn, std::move(topic), std::move(parts), qos);
    } else {
        mSimpleSubscriptions.emplace(std::piecewise_construct,
                                     std::make_tuple(topic),
                                     std::make_tuple(std::reference_wrapper<MQTTClientConnection>(conn), topic, std::vector<std::string>{}, qos));
    }
}
void SubscriptionsManager::deleteSubscription(MQTTClientConnection& conn, const std::string& topic) {
    std::lock_guard<std::shared_mutex> lock{mMutex};
    auto[start, end] = mSimpleSubscriptions.equal_range(topic);
    if(start != end) {
        for(auto it = start; it != end;) {
            if(&it->second.conn.get() == &conn) {
                it = mSimpleSubscriptions.erase(it);
            } else {
                it++;
            }
        }
    } else {
        erase_if(mWildcardSubscriptions, [&conn, &topic](auto& sub) {
            return &sub.conn.get() == &conn && sub.topic == topic;
        });
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
    erase_if(mWildcardSubscriptions, [&conn](auto& sub) {
        return &sub.conn.get() == &conn;
    });
}
void SubscriptionsManager::forEachSubscriber(const std::string& topic, std::function<void(Subscription&)> callback) {
    std::shared_lock<std::shared_mutex> lock{mMutex};
    auto[start, end] = mSimpleSubscriptions.equal_range(topic);
    for(auto it = start; it != end; ++it) {
        callback(it->second);
    }
    for(auto& sub: mWildcardSubscriptions) {
        size_t partIndex = 0;
        bool doesMatch = true;
        splitString(topic, [&] (const auto& actualPart) {
            const auto& expectedPart = sub.topicSplit.at(partIndex);
            if(actualPart == expectedPart || expectedPart == "+") {
                partIndex += 1;
                return IterationDecision::Continue;
            }
            if(expectedPart == "#") {
                partIndex = sub.topicSplit.size();
                return IterationDecision::Stop;
            }
            doesMatch = false;
            return IterationDecision::Stop;
        });
        if(doesMatch && partIndex == sub.topicSplit.size()) {
            callback(sub);
        }

    }
}

}
