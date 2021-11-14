#include "MQTTPersistentState.hpp"
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

static bool doesTopicMatchSubscription(const std::string& topic, const MQTTPersistentState::Subscription& sub) {
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

void MQTTPersistentState::addSubscription(MQTTClientConnection& conn, std::string topic, QoS qos, std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback) {
    addSubscriptionInternal(conn, std::move(topic), qos, std::move(retainedMessageCallback));
}

void MQTTPersistentState::addSubscription(std::string scriptName, std::string topic, std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback) {
    addSubscriptionInternal(std::move(scriptName), std::move(topic), {}, std::move(retainedMessageCallback));

}
void MQTTPersistentState::addSubscriptionInternal(
    std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName> subscriber, std::string topic, std::optional<QoS> qos,
    std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback) {

    std::shared_lock<std::shared_mutex> lock1{ mRetainedMessagesMutex };
    std::lock_guard<std::shared_mutex> lock{ mSubscriptionsMutex };
    auto hasWildcard = std::any_of(topic.begin(), topic.end(), [](char c) {
        return c == '#' || c == '+';
    });
    if(hasWildcard) {
        std::vector<std::string> parts;
        splitString(topic, [&parts](const std::string_view& part) {
            parts.emplace_back(part);
            return IterationDecision::Continue;
        });
        auto& sub = mWildcardSubscriptions.emplace_back(std::move(subscriber), std::move(topic), std::move(parts), qos);
        for(auto& retainedMessage: mRetainedMessages) {
            if(doesTopicMatchSubscription(retainedMessage.first, sub)) {
                retainedMessageCallback(retainedMessage.first, retainedMessage.second.payload);
            }
        }
    } else {
        mSimpleSubscriptions.emplace(std::piecewise_construct,
                                     std::make_tuple(topic),
                                     std::make_tuple(std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName>(std::move(subscriber)), topic, std::vector<std::string>{}, qos));
        auto retainedMessage = mRetainedMessages.find(topic);
        if(retainedMessage != mRetainedMessages.end()) {
            retainedMessageCallback(retainedMessage->first, retainedMessage->second.payload);
        }
    }
}

void MQTTPersistentState::deleteSubscription(std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName> client, const std::string& topic) {
    std::lock_guard<std::shared_mutex> lock{ mSubscriptionsMutex };
    auto[start, end] = mSimpleSubscriptions.equal_range(topic);
    if(start != end) {
        for(auto it = start; it != end;) {
            if(it->second.subscriber == client) {
                it = mSimpleSubscriptions.erase(it);
            } else {
                it++;
            }
        }
    } else {
        erase_if(mWildcardSubscriptions, [&client, &topic](auto& sub) {
            return sub.subscriber == client && sub.topic == topic;
        });
    }
}
void MQTTPersistentState::deleteAllSubscriptions(std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName> client) {
    std::lock_guard<std::shared_mutex> lock{ mSubscriptionsMutex };
    for(auto it = mSimpleSubscriptions.begin(); it != mSimpleSubscriptions.end();) {
        if(it->second.subscriber == client) {
            it = mSimpleSubscriptions.erase(it);
        } else {
            it++;
        }
    }
    erase_if(mWildcardSubscriptions, [&client](auto& sub) {
        return sub.subscriber == client;
    });
}

void MQTTPersistentState::forEachSubscriber(const std::string& topic, std::function<void(Subscription&)> callback) {
    std::shared_lock<std::shared_mutex> lock{ mSubscriptionsMutex };
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
void MQTTPersistentState::retainMessage(std::string&& topic, std::vector<uint8_t>&& payload) {
    std::lock_guard<std::shared_mutex> lock{ mRetainedMessagesMutex };
    mRetainedMessages.erase(topic);
    if(!payload.empty()) {
        mRetainedMessages.emplace(std::piecewise_construct,
                                  std::make_tuple(std::move(topic)),
                                  std::make_tuple(RetainedMessage{std::move(payload)}));
    }
}

}
