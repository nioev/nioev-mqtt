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

static bool doesTopicMatchSubscription(const std::string& topic, const SubscriptionsManager::Subscription& sub) {
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
    return doesMatch && partIndex == sub.topicSplit.size();
}

void SubscriptionsManager::addSubscription(MQTTClientConnection& conn, std::string topic, QoS qos, std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback) {
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
        auto& sub = mWildcardSubscriptions.emplace_back(conn, std::move(topic), std::move(parts), qos);
        for(auto& retainedMessage: mRetainedMessages) {
            if(doesTopicMatchSubscription(retainedMessage.first, sub)) {
                retainedMessageCallback(retainedMessage.first, retainedMessage.second.payload);
            }
        }
    } else {
        mSimpleSubscriptions.emplace(std::piecewise_construct,
                                     std::make_tuple(topic),
                                     std::make_tuple(std::reference_wrapper<MQTTClientConnection>(conn), topic, std::vector<std::string>{}, qos));
        auto retainedMessage = mRetainedMessages.find(topic);
        if(retainedMessage != mRetainedMessages.end()) {
            retainedMessageCallback(retainedMessage->first, retainedMessage->second.payload);
        }
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
        if(doesTopicMatchSubscription(topic, sub)) {
            callback(sub);
        }
    }
}
void SubscriptionsManager::retainMessage(std::string&& topic, std::vector<uint8_t>&& payload) {
    std::lock_guard<std::shared_mutex> lock{mMutex};
    mRetainedMessages.erase(topic);
    if(!payload.empty()) {
        mRetainedMessages.emplace(std::piecewise_construct,
                                  std::make_tuple(std::move(topic)),
                                  std::make_tuple(RetainedMessage{std::move(payload)}));
    }
}

}
