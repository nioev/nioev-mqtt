#include "MQTTPersistentState.hpp"
#include "MQTTClientConnection.hpp"
#include "Util.hpp"

#include <fstream>

namespace nioev {

void MQTTPersistentState::addSubscription(MQTTClientConnection& conn, std::string topic, QoS qos, std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback) {
    std::lock_guard<std::shared_mutex> lock{ mSubscriptionsMutex };
    addSubscriptionInternalNoSubLock(conn, topic, qos, std::move(retainedMessageCallback));
    auto[state, stateLock] = conn.getPersistentState();
    if(state->cleanSession == CleanSession::No) {
        state->subscriptions.emplace_back(PersistentClientState::PersistentSubscription{ std::move(topic), qos });
    }
}

void MQTTPersistentState::addSubscription(std::string scriptName, std::string topic, std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback) {
    std::lock_guard<std::shared_mutex> lock{ mSubscriptionsMutex };
    addSubscriptionInternalNoSubLock(std::move(scriptName), std::move(topic), {}, std::move(retainedMessageCallback));
}
void MQTTPersistentState::addSubscriptionInternalNoSubLock(
    std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName> subscriber, std::string topic, std::optional<QoS> qos,
    std::function<void(const std::string&, const std::vector<uint8_t>&)>&& retainedMessageCallback) {

    std::shared_lock<std::shared_mutex> lock1{ mRetainedMessagesMutex };
    auto hasWildcard = std::any_of(topic.begin(), topic.end(), [](char c) {
        return c == '#' || c == '+';
    });
    if(hasWildcard) {
        std::vector<std::string> parts;
        util::splitString(topic, [&parts](const std::string_view& part) {
            parts.emplace_back(part);
            return util::IterationDecision::Continue;
        });
        auto& sub = mWildcardSubscriptions.emplace_back(std::move(subscriber), std::move(topic), std::move(parts), qos);
        if(retainedMessageCallback) {
           for(auto& retainedMessage: mRetainedMessages) {
               if(util::doesTopicMatchSubscription(retainedMessage.first, sub.topicSplit)) {
                   retainedMessageCallback(retainedMessage.first, retainedMessage.second.payload);
               }
           }
        }
    } else {
        mSimpleSubscriptions.emplace(std::piecewise_construct,
                                     std::make_tuple(topic),
                                     std::make_tuple(std::variant<std::reference_wrapper<MQTTClientConnection>, ScriptName>(std::move(subscriber)), topic, std::vector<std::string>{}, qos));
        if(retainedMessageCallback) {
            auto retainedMessage = mRetainedMessages.find(topic);
            if(retainedMessage != mRetainedMessages.end()) {
                retainedMessageCallback(retainedMessage->first, retainedMessage->second.payload);
            }
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
    if(client.index() == 0) {
        auto& conn = std::get<0>(client).get();
        auto[state, stateLock] = conn.getPersistentState();
        for(auto it = state->subscriptions.begin(); it != state->subscriptions.end(); ++it) {
            if(it->topic == topic) {
                it = state->subscriptions.erase(it);
            } else {
                it++;
            }
        }
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
        if(util::doesTopicMatchSubscription(topic, sub.topicSplit)) {
            callback(sub);
        }
    }
}
void MQTTPersistentState::retainMessage(std::string&& topic, std::vector<uint8_t>&& payload) {
    std::lock_guard<std::shared_mutex> lock{ mRetainedMessagesMutex };
    if(payload.empty()) {
       mRetainedMessages.erase(topic);
       return;
    }
    mRetainedMessages.insert_or_assign(topic, RetainedMessage{std::move(payload)});
}
SessionPresent MQTTPersistentState::loginClient(MQTTClientConnection& conn, std::string&& clientId, CleanSession cleanSession, std::function<void(MQTTClientConnection*)>&& performWill) {
    std::unique_lock<std::recursive_mutex> lock{mPersistentClientStatesMutex};

    constexpr char AVAILABLE_RANDOM_CHARS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";

    decltype(mPersistentClientStates.begin()) existingSession;
    if(clientId.empty()) {
        assert(cleanSession == CleanSession::Yes);
        // generate random client id
        std::string randomId = conn.getTcpClient().getRemoteIp() + ":" + std::to_string(conn.getTcpClient().getRemotePort());
        auto start = randomId.size();
        existingSession = mPersistentClientStates.find(randomId);
        while(existingSession != mPersistentClientStates.end()) {
            std::ifstream urandom{"/dev/urandom"};
            randomId.resize(start + 16);
            for(size_t i = start; i < randomId.size(); ++i) {
                randomId.at(i) = AVAILABLE_RANDOM_CHARS[urandom.get() % strlen(AVAILABLE_RANDOM_CHARS)];
            }
            existingSession = mPersistentClientStates.find(randomId);
        }
        clientId = std::move(randomId);
    } else {
        existingSession = mPersistentClientStates.find(clientId);
    }


    SessionPresent ret = SessionPresent::No;
    auto createNewSession = [&] {
        auto newState = mPersistentClientStates.emplace_hint(existingSession, std::piecewise_construct, std::make_tuple(clientId), std::make_tuple());
        newState->second.clientId = std::move(clientId);
        newState->second.currentClient.store(&conn);
        newState->second.cleanSession = cleanSession;
        conn.setPersistentState(&newState->second);
        ret = SessionPresent::No;
    };

    if(existingSession != mPersistentClientStates.end()) {
        // disconnect existing client
        auto existingClient = existingSession->second.currentClient.load();
        if(existingClient) {
            spdlog::warn("[{}] Already connected, closing old connection", clientId);
            performWill(existingClient);
            existingClient->notifyConnecionError();
            existingSession->second.currentClient = nullptr;
            existingSession->second.lastDisconnectTime = std::chrono::steady_clock::now().time_since_epoch().count();
        }
        if(cleanSession == CleanSession::Yes || existingSession->second.cleanSession == CleanSession::Yes) {
            mPersistentClientStates.erase(existingSession);
            createNewSession();
        } else {
            ret = SessionPresent::Yes;
            existingSession->second.currentClient.store(&conn);
            existingSession->second.cleanSession = cleanSession;
            conn.setPersistentState(&existingSession->second);
            {
                // readd subscriptions
                std::lock_guard<std::shared_mutex> lock2{mSubscriptionsMutex};
                for(auto& sub: existingSession->second.subscriptions) {
                    addSubscriptionInternalNoSubLock(conn, sub.topic, sub.qos, {});
                }
            }
        }
    } else {
        // no session exists
        createNewSession();
    }
    return ret;
}
void MQTTPersistentState::logoutClient(MQTTClientConnection& conn) {
    deleteAllSubscriptions(conn);
    std::unique_lock<std::recursive_mutex> lock{mPersistentClientStatesMutex};
    auto[state, stateLock] = conn.getPersistentState();
    if(!state) {
        return;
    }
    if(state->cleanSession == CleanSession::Yes) {
        mPersistentClientStates.erase(state->clientId);
    } else {
       state->currentClient = nullptr;
       state->lastDisconnectTime = std::chrono::steady_clock::now().time_since_epoch().count();
    }
    stateLock.unlock();
    conn.setPersistentState(nullptr);
}
void MQTTPersistentState::deleteScriptSubscriptions(const ScriptName& script) {
    deleteAllSubscriptions(script);
}

}
