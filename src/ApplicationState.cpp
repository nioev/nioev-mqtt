#include <fstream>
#include "ApplicationState.hpp"
#include "scripting/ScriptContainer.hpp"
#include "scripting/ScriptContainerJS.hpp"
#include "SQLiteCpp/Transaction.h"

namespace nioev {

ApplicationState::ApplicationState()
: mClientManager(*this, 5), mWorkerThread([this]{workerThreadFunc();}), mAsyncPublisher(*this) {
    mTimers.addPeriodicTask(std::chrono::seconds(2), [this] () mutable {
        cleanup();
    });
    mTimers.addPeriodicTask(std::chrono::hours(1), [this] () mutable {
        syncRetainedMessagesToDb();
    });
    // initialize db
    mDb.exec("CREATE TABLE IF NOT EXISTS script (name TEXT UNIQUE PRIMARY KEY NOT NULL, code TEXT NOT NULL, persistent_state TEXT);");
    mDb.exec("CREATE TABLE IF NOT EXISTS retained_msg (topic TEXT UNIQUE PRIMARY KEY NOT NULL, payload BLOB NOT NULL, timestamp TIMESTAMP NOT NULL);");
    mDb.exec("PRAGMA journal_mode=WAL;");
    mQueryInsertScript.emplace(mDb, "INSERT OR REPLACE INTO script (name, code) VALUES (?, ?)");
    mQueryInsertRetainedMsg.emplace(mDb, "INSERT INTO retained_msg (topic, payload, timestamp) VALUES (?, ?, ?)");
    // fetch scripts
    SQLite::Statement scriptQuery(mDb, "SELECT name,code FROM script");
    while(scriptQuery.executeStep()) {
        mQueueInternal.emplace(ChangeRequestAddScript{scriptQuery.getColumn(0), scriptQuery.getColumn(1)});
    }
    // fetch retained messages
    SQLite::Statement retainedMsgQuery(mDb, "SELECT topic,payload,timestamp FROM retained_msg");
    while(retainedMsgQuery.executeStep()) {
        auto payloadColumn = retainedMsgQuery.getColumn(1);
        std::vector<uint8_t> payload{(uint8_t*)payloadColumn.getBlob(), (uint8_t*)payloadColumn.getBlob() + payloadColumn.getBytes()};

        std::stringstream timestampStr{retainedMsgQuery.getColumn(2).getString()};
        struct tm timestamp = { 0 };
        timestampStr >> std::get_time(&timestamp, "%Y-%m-%d %H-%M-%S");
        mRetainedMessages.emplace(retainedMsgQuery.getColumn(0), RetainedMessage{std::move(payload), mktime(&timestamp)});
    }
}
ApplicationState::~ApplicationState() {
    mShouldRun = false;
    mWorkerThread.join();
    syncRetainedMessagesToDb();
}
void ApplicationState::workerThreadFunc() {
    pthread_setname_np(pthread_self(), "app-state");
    auto processInternalQueue = [this] {
        UniqueLockWithAtomicTidUpdate<std::shared_mutex> lock{mMutex, mCurrentRWHolderOfMMutex};
        while(!mQueueInternal.empty()) {
            // don't call executeChangeRequest because that function acquires a lock
            std::visit(*this, std::move(mQueueInternal.front()));
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
    if(mode == RequestChangeMode::SYNC_WHEN_IDLE || mode == RequestChangeMode::SYNC) {
        executeChangeRequest(std::move(changeRequest));
    } else if(mode == RequestChangeMode::ASYNC) {
        /* Because of it's finite size, we can only use the lockless queue if we don't hold the mutex currently; if we push an element
         * while holding the mutex we might hang indefinitly as the worker thread is unable to execute entries from the queue.
         * Note that this also means that we can't hold a read-only version of mMutex, so be cautious when calling requestChange!
         */
        if(mCurrentRWHolderOfMMutex == std::this_thread::get_id()) {
            mQueueInternal.emplace(std::move(changeRequest));
        } else {
            mQueue.push(std::move(changeRequest));
        }

    } else {
        assert(false);
    }
}

void ApplicationState::executeChangeRequest(ChangeRequest&& changeRequest) {
    UniqueLockWithAtomicTidUpdate<std::shared_mutex> lock{mMutex, mCurrentRWHolderOfMMutex};
    std::visit(*this, std::move(changeRequest));
}
void ApplicationState::operator()(ChangeRequestSubscribe&& req) {
    if(req.subType == SubscriptionType::WILDCARD) {
        auto& sub = mWildcardSubscriptions.emplace_back(req.subscriber, req.topic, std::move(req.topicSplit), req.qos);
        for(auto& retainedMessage: mRetainedMessages) {
            if(util::doesTopicMatchSubscription(retainedMessage.first, sub.topicSplit)) {
                sub.subscriber->publish(retainedMessage.first, retainedMessage.second.payload, req.qos, Retained::Yes);
            }
        }

    } else if(req.subType == SubscriptionType::SIMPLE) {
        mSimpleSubscriptions.emplace(std::piecewise_construct,
                                     std::make_tuple(req.topic),
                                     std::make_tuple(req.subscriber, req.topic, std::vector<std::string>{}, req.qos));
        auto retainedMessage = mRetainedMessages.find(req.topic);
        if(retainedMessage != mRetainedMessages.end()) {
            req.subscriber->publish(retainedMessage->first, retainedMessage->second.payload, req.qos, Retained::Yes);
        }
    } else if(req.subType == SubscriptionType::OMNI) {
        auto& sub = mOmniSubscriptions.emplace_back(req.subscriber, req.topic, std::move(req.topicSplit), req.qos);
        for(auto& retainedMessage: mRetainedMessages) {
            sub.subscriber->publish(retainedMessage.first, retainedMessage.second.payload, req.qos, Retained::Yes);
        }
    } else {
        assert(false);
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
    if(req.payload.empty()) {
        mRetainedMessages.erase(req.topic);
    } else {
        mRetainedMessages.insert_or_assign(std::move(req.topic), RetainedMessage{std::move(req.payload), time(nullptr)});
    }
}
void ApplicationState::cleanup() {
    UniqueLockWithAtomicTidUpdate<std::shared_mutex> lock{mMutex, mCurrentRWHolderOfMMutex};
    for(auto it = mClients.begin(); it != mClients.end(); ++it) {
        if(it->get()->getLastDataRecvTimestamp() + (int64_t)it->get()->getKeepAliveIntervalSeconds() * 2'000'000'000 <= std::chrono::steady_clock::now().time_since_epoch().count()) {
            // timeout
            logoutClient(*it->get());
        }
    }
    lock.unlock();
    // delete disconnected clients
    mClientManager.suspendAllThreads();
    lock.lock();
    for(auto it = mClients.begin(); it != mClients.end();) {
        if((*it)->isLoggedOut()) {
            it = mClients.erase(it);
        } else {
            it++;
        }
    }
    mClientManager.resumeAllThreads();
}
void ApplicationState::operator()(ChangeRequestLoginClient&& req) {
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
            spdlog::warn("[{}] Already logged in, closing old connection", req.clientId);
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
                requestChange(ChangeRequestSubscribe{req.client, sub.topic, util::splitTopics(sub.topic), util::hasWildcard(sub.topic) ? SubscriptionType::WILDCARD : SubscriptionType::SIMPLE, sub.qos});
            }
        }
    } else {
        // no session exists
        createNewSession();
    }

    // send CONNACK now
    // we need to do it here because only here we now the value of the session present flag
    // we could use callbacks, but that seems too complicated
    spdlog::info("[{}] Logged in from [{}:{}]", req.client->getClientId(), req.client->getTcpClient().getRemoteIp(), req.client->getTcpClient().getRemotePort());
    util::BinaryEncoder response;
    response.encodeByte(static_cast<uint8_t>(MQTTMessageType::CONNACK) << 4);
    response.encodeByte(2); // remaining packet length
    response.encodeByte(sessionPresent == SessionPresent::Yes ? 1 : 0);
    response.encodeByte(0); // everything okay


    req.client->sendData(response.moveData());
    req.client->setState(MQTTClientConnection::ConnectionState::CONNECTED);
}
void ApplicationState::operator()(ChangeRequestLogoutClient&& req) {
    logoutClient(*req.client);
}
void ApplicationState::operator()(ChangeRequestAddScript&& req) {
    auto existingScript = mScripts.find(req.name);
    if(existingScript != mScripts.end()) {
        deleteScript(existingScript);
    }
    std::shared_ptr<ScriptContainer> script;
    auto extension = util::getFileExtension(req.name);
    if(extension == ".js") {
        script = std::make_shared<ScriptContainerJS>(*this, req.name, std::move(req.code));
    } else {
        req.statusOutput.error(req.name, "Invalid script filename: " + req.name);
        return;
    }
    mScripts.emplace(req.name, script);
    script->init(std::move(req.statusOutput));
}
void ApplicationState::deleteScript(std::unordered_map<std::string, std::shared_ptr<ScriptContainer>>::iterator it) {
    if(it == mScripts.end())
        return;
    it->second->forceQuit();
    deleteAllSubscriptions(*it->second);
    mScripts.erase(it);
}
void ApplicationState::logoutClient(MQTTClientConnection& client) {
    if(client.isLoggedOut())
        return;
    mClientManager.removeClientConnection(client);
    {
        // perform will
        auto willMsg = client.moveWill();
        if(willMsg) {
            publishInternal(std::move(willMsg->topic), std::move(willMsg->msg), willMsg->qos, willMsg->retain);
        }
    }
    deleteAllSubscriptions(client);
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
    client.notifyLoggedOut();
}
void ApplicationState::publish(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain) {
    std::shared_lock<std::shared_mutex> lock{ mMutex };
    publishNoLockNoRetain(topic, msg, qos, retain);
    lock.unlock();
    if(retain == Retain::Yes) {
        // we aren't allowed to call requestChange from another thread while holding a lock, so we need to do it here
        requestChange(ChangeRequestRetain{std::move(topic), std::move(msg)});
    }
}
void ApplicationState::publishInternal(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain) {

    publishNoLockNoRetain(topic, msg, qos, retain);
    if(retain == Retain::Yes) {
        requestChange(ChangeRequestRetain{std::move(topic), std::move(msg)});
    }
}
void ApplicationState::publishNoLockNoRetain(const std::string& topic, const std::vector<uint8_t>& msg, std::optional<QoS> qos, Retain retain) {
    // NOTE: It's possible that we only have a read-only lock here, so we aren't allowed to call requestChange
    #ifndef NDEBUG
        if(topic != LOG_TOPIC) {
            std::string dataAsStr{msg.begin(), msg.end()};
            spdlog::info("Publishing on '{}' data '{}'", topic, dataAsStr);
        }
    #endif
    // first check for publish to $NIOEV
    if(util::startsWith(topic, "$NIOEV")) {
        //performSystemAction(topic, msg);
    }
    // second run scripts
    auto action = SyncAction::Continue;
    forEachSubscriberThatIsOfT<ScriptContainer>(topic, [&topic, &msg, &action](Subscription& sub) {
        sub.subscriber->publish(topic, msg, *sub.qos, Retained::No);
        // TODO reimplement sync scripts
        // if(runScriptWithPublishedMessage(std::get<MQTTPersistentState::ScriptName>(sub.subscriber), topic, msg, Retained::No) == SyncAction::AbortPublish) {
        //    action = SyncAction::AbortPublish;
        // }
    });
    if(action == SyncAction::AbortPublish) {
        return;
    }
    // then send to clients
    // this order is neccessary to allow the scripts to abort the message delivery to clients
    forEachSubscriberThatIsNotOfT<ScriptContainer>(
        topic, [&topic, &msg](Subscription& sub) { sub.subscriber->publish(topic, msg, *sub.qos, Retained::No); });

}
void ApplicationState::handleNewClientConnection(TcpClientConnection&& conn) {
    UniqueLockWithAtomicTidUpdate<std::shared_mutex> lock{mMutex, mCurrentRWHolderOfMMutex};
    spdlog::info("New connection from [{}:{}]", conn.getRemoteIp(), conn.getRemotePort());
    auto newClient = mClients.emplace_back(std::make_shared<MQTTClientConnection>(*this, std::move(conn)));
    mClientManager.addClientConnection(*newClient);
}
void ApplicationState::deleteAllSubscriptions(Subscriber& sub) {
    for(auto it = mSimpleSubscriptions.begin(); it != mSimpleSubscriptions.end();) {
        if(it->second.subscriber.get() == &sub) {
            it = mSimpleSubscriptions.erase(it);
        } else {
            it++;
        }
    }
    erase_if(mWildcardSubscriptions, [&sub](auto& subscription) {
        return subscription.subscriber.get() == &sub;
    });
}
ApplicationState::ScriptsInfo ApplicationState::getScriptsInfo() {
    std::shared_lock<std::shared_mutex> lock{mMutex};
    ScriptsInfo ret;
    for(auto& script: mScripts) {
        ScriptsInfo::ScriptInfo scriptInfo;
        scriptInfo.name = script.first;
        scriptInfo.code = script.second->getCode();
        ret.scripts.emplace_back(std::move(scriptInfo));
    }
    return ret;
}
void ApplicationState::syncRetainedMessagesToDb() {
    UniqueLockWithAtomicTidUpdate lock{mMutex, mCurrentRWHolderOfMMutex};
    SQLite::Transaction transaction{mDb};
    mDb.exec("DELETE FROM retained_msg");
    for(auto& msg: mRetainedMessages) {
        mQueryInsertRetainedMsg->bindNoCopy(1, msg.first);
        mQueryInsertRetainedMsg->bindNoCopy(2, msg.second.payload.data(), msg.second.payload.size());
        struct tm res;
        gmtime_r(&msg.second.timestamp, &res);
        std::stringstream timestampAsStr;
        timestampAsStr << std::put_time(&res, "%Y-%m-%d %H-%M-%S.000");
        mQueryInsertRetainedMsg->bind(3, timestampAsStr.str());

        mQueryInsertRetainedMsg->exec();
        mQueryInsertRetainedMsg->clearBindings();
        mQueryInsertRetainedMsg->reset();
    }
    transaction.commit();
    mDb.exec("VACUUM");
    spdlog::info("Synced retained messages to db");
}
void ApplicationState::addScript(
    std::string name, std::function<void(const std::string&)>&& onSuccess, std::function<void(const std::string&, const std::string&)>&& onError,
    std::string code) {
    mQueryInsertScript->bindNoCopy(1, name);
    mQueryInsertScript->bindNoCopy(2, code);
    mQueryInsertScript->exec();
    mQueryInsertScript->reset();
    mQueryInsertScript->clearBindings();
    ScriptStatusOutput statusOutput;
    statusOutput.success = std::move(onSuccess);
    auto originalError = std::move(statusOutput.error);
    statusOutput.error = [onError = std::move(onError), originalError = std::move(originalError)] (auto& scriptName, const auto& error) {
        originalError(scriptName, error);
        onError(scriptName, error);
    };
    requestChange(ChangeRequestAddScript{std::move(name), std::move(code), std::move(statusOutput)});
}
}

