#include <fstream>
#include "ApplicationState.hpp"
#include "scripting/ScriptContainer.hpp"

namespace nioev {

ApplicationState::ApplicationState()
: mClientManager(*this, 5), mWorkerThread([this]{workerThreadFunc();}) {
    int tries = 0;
    mTimers.addPeriodicTask(std::chrono::seconds(2), [this, tries] mutable {
        if(tries < 10) {
            if(mMutex.try_lock()) {
                cleanup();
                mMutex.unlock();
                tries = 0;
            } else {
                tries += 1;
            }
        } else {
            tries = -5;
            mQueue.push(ChangeRequestCleanup{});
        }
    });
}
ApplicationState::~ApplicationState() {
    mShouldRun = false;
    mWorkerThread.join();
}
void ApplicationState::workerThreadFunc() {
    auto processInternalQueue = [this] {
        std::unique_lock<std::shared_mutex> lock{mMutex};
        while(!mQueueInternal.empty()) {
            executeChangeRequest(std::move(mQueueInternal.front()));
            mQueueInternal.pop();
        }
    };
    while(mShouldRun) {
        processInternalQueue();
        while(!mQueue.was_empty()) {
            processInternalQueue();
            executeChangeRequest(mQueue.pop());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void ApplicationState::requestChange(ChangeRequest&& changeRequest, ApplicationState::RequestChangeMode mode) {
    if(mode == RequestChangeMode::SYNC_WHEN_IDLE) {
        executeChangeRequest(std::move(changeRequest));
    } else if(mode == RequestChangeMode::ASYNC) {
        mQueue.push(std::move(changeRequest));
    } else if(mode == RequestChangeMode::SYNC) {
        // TODO smarter algorithm here
        executeChangeRequest(std::move(changeRequest));
    } else {
        assert(false);
    }
}

void ApplicationState::executeChangeRequest(ChangeRequest&& changeRequest) {
    std::visit(*this, std::move(changeRequest));
}
void ApplicationState::operator()(ChangeRequestSubscribe&& req) {
    std::unique_lock<std::shared_mutex> lock{mMutex};
    if(req.isWildcardSub) {
        auto& sub = mWildcardSubscriptions.emplace_back(req.subscriber, req.topic, std::move(req.topicSplit), req.qos);
        for(auto& retainedMessage: mRetainedMessages) {
            if(util::doesTopicMatchSubscription(retainedMessage.first, sub.topicSplit)) {
                sub.subscriber->publish(retainedMessage.first, retainedMessage.second.payload, req.qos, Retained::Yes);
            }
        }

    } else {
        mSimpleSubscriptions.emplace(std::piecewise_construct,
                                     std::make_tuple(req.topic),
                                     std::make_tuple(req.subscriber, req.topic, std::vector<std::string>{}, req.qos));
        auto retainedMessage = mRetainedMessages.find(req.topic);
        if(retainedMessage != mRetainedMessages.end()) {
            req.subscriber->publish(retainedMessage->first, retainedMessage->second.payload, req.qos, Retained::Yes);
        }
    }
    auto subscriberAsMQTTConn = std::dynamic_pointer_cast<MQTTClientConnection>(req.subscriber);
    if(subscriberAsMQTTConn) {
        auto[state, stateLock] = subscriberAsMQTTConn->getPersistentState();
        if(state && state->cleanSession == CleanSession::No) {
            state->subscriptions.emplace_back(PersistentClientState::PersistentSubscription{ std::move(req.topic), req.qos });
        }
    }
}
void ApplicationState::operator()(ChangeRequestUnsubscribe&& req) {
    std::unique_lock<std::shared_mutex> lock{mMutex};
    auto[start, end] = mSimpleSubscriptions.equal_range(req.topic);
    if(start != end) {
        for(auto it = start; it != end;) {
            if(it->second.subscriber == req.subscriber) {
                it = mSimpleSubscriptions.erase(it);
            } else {
                it++;
            }
        }
    } else {
        erase_if(mWildcardSubscriptions, [&req](auto& sub) {
            return sub.subscriber == req.subscriber && sub.topic == req.topic;
        });
    }
    auto subscriberAsMQTTConn = std::dynamic_pointer_cast<MQTTClientConnection>(req.subscriber);
    if(subscriberAsMQTTConn) {
        auto[state, stateLock] = subscriberAsMQTTConn->getPersistentState();
        if(state->cleanSession == CleanSession::No) {
            for(auto it = state->subscriptions.begin(); it != state->subscriptions.end(); ++it) {
                if(it->topic == req.topic) {
                    it = state->subscriptions.erase(it);
                } else {
                    it++;
                }
            }
        }
    }
}
void ApplicationState::operator()(ChangeRequestRetain&& req) {
    std::unique_lock<std::shared_mutex> lock{mMutex};
    mRetainedMessages.emplace(std::move(req.topic), RetainedMessage{std::move(req.payload)});
}
void ApplicationState::operator()(ChangeRequestCleanup&& req) {
    std::unique_lock<std::shared_mutex> lock{mMutex};
    cleanup();
}
void ApplicationState::cleanup() {
    for(auto it = mClients.begin(); it != mClients.end(); ++it) {
        auto[recvData, recvDataLock] = (*it)->getRecvData();
        if(recvData.get().lastDataReceivedTimestamp + (int64_t)it->get()->getKeepAliveIntervalSeconds() * 2'000'000'000 <= std::chrono::steady_clock::now().time_since_epoch().count()) {
            // timeout
            logoutClient(*it->get());
        }
    }

    // delete disconnected clients
    mClientManager.suspendAllThreads();
    for(auto it = mClients.begin(); it != mClients.end();) {
        if((*it)->shouldBeDisconnected()) {
            it = mClients.erase(it);
        } else {
            it++;
        }
    }
    mClientManager.resumeAllThreads();
}
void ApplicationState::operator()(ChangeRequestDisconnectClient&& req) {
    std::unique_lock<std::shared_mutex> lock{mMutex};
    logoutClient(*req.client);
}
void ApplicationState::operator()(ChangeRequestLoginClient&& req) {
    std::unique_lock<std::shared_mutex> lock{mMutex};
    constexpr char AVAILABLE_RANDOM_CHARS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";

    decltype(mPersistentClientStates.begin()) existingSession;
    if(req.clientId.empty()) {
        assert(req.cleanSession == CleanSession::Yes);
        // generate random client id
        std::string randomId = req.client->getTcpClient().getRemoteIp() + ":" + std::to_string(req.client->getTcpClient().getRemotePort());
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
        req.clientId = std::move(randomId);
    } else {
        existingSession = mPersistentClientStates.find(req.clientId);
    }


    SessionPresent sessionPresent = SessionPresent::No;
    auto createNewSession = [&] {
        auto newState = mPersistentClientStates.emplace_hint(existingSession, std::piecewise_construct, std::make_tuple(req.clientId), std::make_tuple());
        req.client->setClientId(req.clientId);
        newState->second.clientId = std::move(req.clientId);
        newState->second.currentClient = req.client.get();
        newState->second.cleanSession = req.cleanSession;
        req.client->setPersistentState(&newState->second);
        sessionPresent = SessionPresent::No;
    };

    if(existingSession != mPersistentClientStates.end()) {
        // disconnect existing client
        auto existingClient = existingSession->second.currentClient;
        if(existingClient) {
            spdlog::warn("[{}] Already connected, closing old connection", req.clientId);
            logoutClient(*existingClient);
        }
        if(req.cleanSession == CleanSession::Yes || existingSession->second.cleanSession == CleanSession::Yes) {
            createNewSession();
        } else {
            sessionPresent = SessionPresent::Yes;
            req.client->setClientId(req.clientId);
            existingSession->second.currentClient = req.client.get();
            existingSession->second.cleanSession = req.cleanSession;
            req.client->setPersistentState(&existingSession->second);
            for(auto& sub: existingSession->second.subscriptions) {
                mQueueInternal.emplace(ChangeRequestSubscribe{req.client, sub.topic, util::splitTopics(sub.topic), util::hasWildcard(sub.topic), sub.qos});
            }
        }
    } else {
        // no session exists
        createNewSession();
    }

    // send CONNACK now
    // we need to do it here because only here we now the value of the session present flag
    // we could use callbacks, but that seems too complicated
    spdlog::info("[{}] Connected", req.client->getClientId());
    util::BinaryEncoder response;
    response.encodeByte(static_cast<uint8_t>(MQTTMessageType::CONNACK) << 4);
    response.encodeByte(2); // remaining packet length
    response.encodeByte(sessionPresent == SessionPresent::Yes ? 1 : 0);
    response.encodeByte(0); // everything okay


    req.client->sendData(response.moveData());
    req.client->setState(MQTTClientConnection::ConnectionState::CONNECTED);
}
void ApplicationState::logoutClient(MQTTClientConnection& client) {
    mClientManager.removeClientConnection(client);
    {
        // perform will
        auto willMsg = client.moveWill();
        if(willMsg) {
            publishWithoutAcquiringMutex(std::move(willMsg->topic), std::move(willMsg->msg), willMsg->qos, willMsg->retain);
        }
    }
    {
        // delete all subs
        for(auto it = mSimpleSubscriptions.begin(); it != mSimpleSubscriptions.end();) {
            if(it->second.subscriber.get() == &client) {
                it = mSimpleSubscriptions.erase(it);
            } else {
                it++;
            }
        }
        erase_if(mWildcardSubscriptions, [&client](auto& sub) {
            return sub.subscriber.get() == static_cast<Subscriber*>(&client);
        });
    }
    {
        // detach persistent state
        auto[state, stateLock] = client.getPersistentState();
        if(!state) {
            return;
        }
        if(state->cleanSession == CleanSession::Yes) {
            mPersistentClientStates.erase(state->clientId);
        } else {
            state->currentClient = nullptr;
            state->lastDisconnectTime = std::chrono::steady_clock::now().time_since_epoch().count();
        }
        static_assert(std::is_reference<decltype(state)>::value);
        state = nullptr;
    }
    spdlog::info("[{}] Logged out", client.getClientId());
    client.getTcpClient().close();
    client.notifyConnecionError();
}
void ApplicationState::publish(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain) {
    std::shared_lock<std::shared_mutex> lock{ mMutex };
    publishWithoutAcquiringMutex(std::move(topic), std::move(msg), qos, retain);
}
void ApplicationState::publishWithoutAcquiringMutex(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain) {
#ifndef NDEBUG
    {
        std::string dataAsStr{msg.begin(), msg.end()};
        spdlog::info("Publishing on '{}' data '{}'", topic, dataAsStr);
    }
#endif
    // first check for publish to $NIOEV
    if(util::startsWith(topic, "$NIOEV")) {
        //performSystemAction(topic, msg);
    }
    // second run scripts
    // TODO optimize forEachSubscriber so that it skips other subscriptions automatically, avoiding unnecessary work checking for all matches
    auto action = SyncAction::Continue;
    forEachSubscriber<ScriptContainer>(topic, [this, &topic, &msg, &action] (Subscription& sub) {
        sub.subscriber->publish(topic, msg, *sub.qos, Retained::No);
        // TODO reimplement sync scripts
        //if(runScriptWithPublishedMessage(std::get<MQTTPersistentState::ScriptName>(sub.subscriber), topic, msg, Retained::No) == SyncAction::AbortPublish) {
        //    action = SyncAction::AbortPublish;
       // }
    });
    if(action == SyncAction::AbortPublish) {
        return;
    }
    // then send to clients
    // this order is neccessary to allow the scripts to abort the message delivery to clients
    forEachSubscriber(topic, [this, &topic, &msg] (Subscription& sub) {
        sub.subscriber->publish(topic, msg, *sub.qos, Retained::No);
    });
    if(retain == Retain::Yes) {
        mQueueInternal.emplace(ChangeRequestRetain{std::move(topic), std::move(msg)});
    }
}
void ApplicationState::handleNewClientConnection(TcpClientConnection&& conn) {
    std::lock_guard<std::shared_mutex> lock{mClientsMutex};
    spdlog::info("New connection from [{}:{}]", conn.getRemoteIp(), conn.getRemotePort());
    auto newClient = mClients.emplace_back(std::make_shared<MQTTClientConnection>(std::move(conn)));
    mClientManager.addClientConnection(*newClient);
}
}

