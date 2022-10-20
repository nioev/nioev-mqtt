#include <fstream>
#include "ApplicationState.hpp"
#include "scripting/ScriptContainer.hpp"
#include "scripting/ScriptContainerJS.hpp"
#include "SQLiteCpp/Transaction.h"
#include "MQTTPublishPacketBuilder.hpp"
#include "Statistics.hpp"
#include "spdlog/sinks/base_sink.h"
#include "spdlog/pattern_formatter.h"

namespace nioev::mqtt {

thread_local std::unique_ptr<spdlog::formatter> tFormatter;
class LogSink : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
public:
    LogSink(ApplicationState& app) : mApp(app) {
        set_pattern_(nioev::lib::LOG_PATTERN);
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        if(!tFormatter)
            tFormatter.reset(new spdlog::pattern_formatter(nioev::lib::LOG_PATTERN));
        tFormatter->format(msg, formatted);
        assert(formatted.size() > 0);
        std::vector<uint8_t> formattedBuffer((uint8_t*)formatted.begin(), (uint8_t*)formatted.end() - 1);
        mApp.publishAsync(MQTTPacket{ LOG_TOPIC, std::move(formattedBuffer), QoS::QoS0, Retain::No });
    }
    void flush_() override { }
    ApplicationState& mApp;
};

ApplicationState::ApplicationState() : mAsyncPublisher(*this), mStatistics(std::make_shared<Statistics>(*this)), mClientManager(*this), mWorkerThread([this] { workerThreadFunc(); }) {
    spdlog::default_logger()->sinks().push_back(std::make_shared<LogSink>(*this));
    mStatistics->init();
    mTimers.addPeriodicTask(std::chrono::seconds(2), [this]() mutable { cleanup(); });
    mTimers.addPeriodicTask(std::chrono::minutes(10), [this]() mutable { syncRetainedMessagesToDb(); });
    // initialize db
    mDb.exec("CREATE TABLE IF NOT EXISTS script (name TEXT UNIQUE PRIMARY KEY NOT NULL, code TEXT NOT NULL, persistent_state TEXT, active BOOL NOT NULL DEFAULT TRUE);");
    mDb.exec("CREATE TABLE IF NOT EXISTS retained_msg (topic TEXT UNIQUE PRIMARY KEY NOT NULL, payload BLOB NOT NULL, timestamp TIMESTAMP NOT NULL, qos INTEGER NOT NULL);");
    mDb.exec("PRAGMA journal_mode=WAL;");
    mQueryInsertScript.emplace(mDb, "INSERT OR REPLACE INTO script (name, code) VALUES (?, ?)");
    mQueryInsertRetainedMsg.emplace(mDb, "INSERT INTO retained_msg (topic, payload, timestamp, qos) VALUES (?, ?, ?, ?)");
    // fetch scripts
    SQLite::Statement scriptQuery(mDb, "SELECT name,code,active FROM script");
    std::vector<std::tuple<std::string, std::string, bool>> scripts;
    while(scriptQuery.executeStep()) {
        scripts.emplace_back(scriptQuery.getColumn(0), scriptQuery.getColumn(1), scriptQuery.getColumn(2).getInt());
    }
    std::sort(scripts.begin(), scripts.end(), [](auto& a, auto& b) -> bool {
        if(getFileExtension(std::get<0>(a)) == ".cpp") {
            return true;
        } else if(getFileExtension(std::get<0>(b)) == ".cpp") {
            return false;
        }
        return true;
    });
    mAmountOfScriptsLoadedFromDBOnStartup = scripts.size();
    for(auto& [name, code, active] : scripts) {
        executeChangeRequest(ChangeRequestAddScript{ name, std::move(code), ScriptStatusOutput{}, true });
        if(!active) {
            executeChangeRequest(ChangeRequestDeactivateScript{ std::move(name) });
        }
    }
    UniqueLockWithAtomicTidUpdate<std::shared_mutex> lock{ mMutex, mCurrentRWHolderOfMMutex };
    // fetch retained messages
    SQLite::Statement retainedMsgQuery(mDb, "SELECT topic,payload,timestamp,qos FROM retained_msg");
    while(retainedMsgQuery.executeStep()) {
        auto payloadColumn = retainedMsgQuery.getColumn(1);
        std::vector<uint8_t> payload{ (uint8_t*)payloadColumn.getBlob(), (uint8_t*)payloadColumn.getBlob() + payloadColumn.getBytes() };

        std::stringstream timestampStr{ retainedMsgQuery.getColumn(2).getString() };
        struct tm timestamp = { 0 };
        timestampStr >> std::get_time(&timestamp, "%Y-%m-%d %H-%M-%S");
        mRetainedMessages.emplace(retainedMsgQuery.getColumn(0), RetainedMessage{ std::move(payload), mktime(&timestamp), static_cast<QoS>(retainedMsgQuery.getColumn(3).getInt()) /* FIXME: PROPERTIES */ });
    }
}
ApplicationState::~ApplicationState() {
    mShouldRun = false;
    mWorkerThread.join();
    syncRetainedMessagesToDb();
}
void ApplicationState::workerThreadFunc() {
    pthread_setname_np(pthread_self(), "app-state");
    uint sleepCounter = 0;

    uint tasksPerformed = 0;
    auto processInternalQueue = [this, &tasksPerformed] {
        for(auto& client : mClients) {
            if(client.hasSendError()) {
                logoutClient(client);
            }
        }
        while(!mQueueInternal.empty()) {
            executeChangeRequest(std::move(mQueueInternal.front()));
            mQueueInternal.pop_front();
            tasksPerformed += 1;
        }
    };
    uint yieldHelpCount = 0;
    while(mShouldRun) {
        UniqueLockWithAtomicTidUpdate<std::shared_mutex> lock{ mMutex, mCurrentRWHolderOfMMutex };
        tasksPerformed = 0;
        while(!mQueue.was_empty()) {
            executeChangeRequest(mQueue.pop());
            tasksPerformed += 1;
            processInternalQueue();
        }
        if(tasksPerformed == 0) {
            processInternalQueue();
        }
        lock.unlock();
        mCurrentRWHolderOfMMutex = std::thread::id();
        if(tasksPerformed == 0) {
            sleepCounter += 1;
            switch(mWorkerThreadSleepLevel) {
            case WorkerThreadSleepLevel::YIELD:
                std::this_thread::yield();
                if(sleepCounter >= 5) {
                    // tests show that after 5 yields, we only very rarely find something in the queue again (at least on my system)
                    // maybe we should make these points configurable or adjust them dynamically?
                    spdlog::debug("Switching to 10µs sleep interval");
                    mWorkerThreadSleepLevel = WorkerThreadSleepLevel::MICROSECONDS;
                    sleepCounter = 0;
                }
                break;
            case WorkerThreadSleepLevel::MICROSECONDS:
                std::this_thread::sleep_for(std::chrono::microseconds(10));
                if(sleepCounter > 200) {
                    spdlog::debug("Switching to 1ms sleep interval");
                    mWorkerThreadSleepLevel = WorkerThreadSleepLevel::MILLISECONDS;
                    sleepCounter = 0;
                }
                break;
            case WorkerThreadSleepLevel::MILLISECONDS:
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if(sleepCounter > 100) {
                    spdlog::debug("Switching to 10ms sleep interval");
                    mWorkerThreadSleepLevel = WorkerThreadSleepLevel::TENS_OF_MILLISECONDS;
                    sleepCounter = 0;
                }
                break;
            case WorkerThreadSleepLevel::TENS_OF_MILLISECONDS:
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                break;
            }
        } else {
            if(mWorkerThreadSleepLevel == WorkerThreadSleepLevel::YIELD)
                spdlog::debug("Yield helped {} sleep level: {}", yieldHelpCount++, sleepCounter);
            sleepCounter = 0;
            if(static_cast<uint>(mWorkerThreadSleepLevel.load()) > static_cast<uint>(WorkerThreadSleepLevel::YIELD)) {
                mWorkerThreadSleepLevel = static_cast<WorkerThreadSleepLevel>(static_cast<uint>(mWorkerThreadSleepLevel.load()) - 1);
                spdlog::debug("Decreasing sleep interval");
                // TODO add a configuration option for always jumping immediately to YIELD instead of slowly scaling down
            }
        }
    }
}

void ApplicationState::requestChange(ChangeRequest&& changeRequest, ApplicationState::RequestChangeMode mode) {
    std::visit(overloaded{
                   [&](const ChangeRequestLoginClient& req) { req.client->incTaskQueueRefCount(); },
                   [&](const ChangeRequestLogoutClient& req) { req.client->incTaskQueueRefCount(); },
                   [&](const ChangeRequestSubscribe& req) { req.subscriber->incTaskQueueRefCount(); },
                   [&](const ChangeRequestUnsubscribe& req) { req.subscriber->incTaskQueueRefCount(); },
                   [&](const ChangeRequestUnsubscribeFromAll& req) { req.subscriber->incTaskQueueRefCount(); },
                   [&](auto&) {} }, changeRequest);

    if(mode == RequestChangeMode::SYNC_WHEN_IDLE || mode == RequestChangeMode::SYNC) {
        UniqueLockWithAtomicTidUpdate<std::shared_mutex> lock{ mMutex, mCurrentRWHolderOfMMutex };
        executeChangeRequest(std::move(changeRequest));
    } else if(mode == RequestChangeMode::ASYNC) {
        /* Because of it's finite size, we can only use the lockless queue if we don't hold the mutex currently; if we push an element
         * while holding the mutex we might hang indefinitly as the worker thread is unable to execute entries from the queue.
         * Note that this also means that we can't hold a read-only version of mMutex, so be cautious when calling requestChange!
         */
        if(mCurrentRWHolderOfMMutex == std::this_thread::get_id()) {
            mQueueInternal.emplace_back(std::move(changeRequest));
        } else {
            mQueue.push(std::move(changeRequest));
        }

    } else {
        assert(false);
    }
}

void ApplicationState::executeChangeRequest(ChangeRequest&& changeRequest) {
    std::visit(*this, std::move(changeRequest));
    std::visit(overloaded{
                   [&](const ChangeRequestLoginClient& req) {
                       if(req.client->decTaskQueueRefCount() && req.client->isLoggedOut())
                           mShouldCleanup = true;
                   },
                   [&](const ChangeRequestLogoutClient& req) {
                       if(req.client->decTaskQueueRefCount() && req.client->isLoggedOut())
                           mShouldCleanup = true;
                   },
                   [&](const ChangeRequestSubscribe& req) {
                       if(req.subscriber->decTaskQueueRefCount() && req.subscriber->isDeleted())
                           mShouldCleanup = true;
                   },
                   [&](const ChangeRequestUnsubscribe& req) {
                       if(req.subscriber->decTaskQueueRefCount() && req.subscriber->isDeleted())
                           mShouldCleanup = true;
                   },
                   [&](const ChangeRequestUnsubscribeFromAll& req) {
                       if(req.subscriber->decTaskQueueRefCount() && req.subscriber->isDeleted())
                           mShouldCleanup = true;
                   },
                   [&](auto&) {} }, changeRequest);
}

static inline void sendPublish(Subscriber& sub, const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained, const PropertyList& properties) {
    MQTTPublishPacketBuilder builder{ topic, payload, retained, properties };
    sub.publish(topic, payload, qos, retained, properties, builder);
}

void ApplicationState::subscribeClientInternal(ChangeRequestSubscribe&& req, ShouldPersistSubscription persist) {
    Subscription sub{req.subscriber, req.qos};
    mSubscriptions.addSubscription(req.topic, sub);
    for(auto& retainedMessage : mRetainedMessages) {
        mSubscriptions.forEveryMatch(retainedMessage.first, [&](Subscription& matchedSub) {
            if(sub == matchedSub) {
                sendPublish(*req.subscriber, retainedMessage.first, retainedMessage.second.payload, minQoS(retainedMessage.second.qos, req.qos), Retained::Yes, retainedMessage.second.properties);
            }
        });
    }
}
void ApplicationState::operator()(ChangeRequestSubscribe&& req) {
    if(req.subscriber->isDeleted())
        return;
    subscribeClientInternal(std::move(req), ShouldPersistSubscription::Yes);
}
void ApplicationState::operator()(ChangeRequestUnsubscribe&& req) {
    if(req.subscriber->isDeleted())
        return;
    mSubscriptions.removeSubscription(req.topic, Subscription{req.subscriber, QoS::QoS0 /* QoS doesn't matter here */});
}
void ApplicationState::operator()(ChangeRequestUnsubscribeFromAll&& req) {
    if(req.subscriber->isDeleted())
        return;
    deleteAllSubscriptions(*req.subscriber);
}
void ApplicationState::operator()(ChangeRequestRetain&& req) {
    if(req.packet.payload.empty()) {
        mRetainedMessages.erase(req.packet.topic);
    } else {
        mRetainedMessages.insert_or_assign(std::move(req.packet.topic), RetainedMessage{ std::move(req.packet.payload), time(nullptr), req.packet.qos, req.packet.properties });
    }
}
void ApplicationState::cleanup() {
    UniqueLockWithAtomicTidUpdate<std::shared_mutex> lock{ mMutex, mCurrentRWHolderOfMMutex };
    for(auto it = mClients.begin(); it != mClients.end(); ++it) {
        if(!it->isLoggedOut()
           && it->getLastDataRecvTimestamp() + (int64_t)it->getKeepAliveIntervalSeconds() * 2'000'000'000 <= std::chrono::steady_clock::now().time_since_epoch().count()) {
            spdlog::info(
                "[{}] Timeout after {} seconds, keep alive is {}", it->getClientId(),
                (std::chrono::steady_clock::now().time_since_epoch().count() - it->getLastDataRecvTimestamp()) / 1'000'000'000, it->getKeepAliveIntervalSeconds());
            logoutClient(*it);
        }
    }
    // Delete disconnected clients.
    // As we need to suspend all client threads for this, this operation is really expensive, so we delete the clients if there are actually ones
    // that need to be deleted.
    if(mShouldCleanup) {
        lock.unlock();
        mCurrentRWHolderOfMMutex = std::thread::id();
        mClientManager.suspendAllThreads();
        lock.lock();
        mCurrentRWHolderOfMMutex = std::this_thread::get_id();
        for(auto it = mClients.begin(); it != mClients.end();) {
            if(it->isLoggedOut() && it->getTaskQueueRefCount() == 0) {
                it = mClients.erase(it);
            } else {
                it++;
            }
        }
        for(auto it = mDeletedPersistentClientStates.begin(); it != mDeletedPersistentClientStates.end();) {
            if((**it).getTaskQueueRefCount() == 0) {
                it = mDeletedPersistentClientStates.erase(it);
            } else {
                it++;
            }
        }
        mShouldCleanup = false;
        mClientManager.resumeAllThreads();
    }
}
void ApplicationState::operator()(ChangeRequestLoginClient&& req) {
    if(req.client->isLoggedOut())
        return;
    constexpr char AVAILABLE_RANDOM_CHARS[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";

    decltype(mPersistentClientStates.begin()) existingSession;
    if(req.clientId.empty()) {
        assert(req.cleanSession == CleanSession::Yes);
        // generate random client id
        std::string randomId = req.client->getTcpClient().getRemoteIp() + ":" + std::to_string(req.client->getTcpClient().getRemotePort());
        auto start = randomId.size();
        existingSession = mPersistentClientStates.find(randomId);
        while(existingSession != mPersistentClientStates.end()) {
            std::ifstream urandom{ "/dev/urandom" };
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

    // this part is a lambda because if we need to send a client QoS1 packets that it missed, we need to send CONNACK before that, but that part of
    // the code is in an if-statement... it's a bit messy, TODO cleanup?
    bool connackSent = false;
    auto sendConnack = [&] {
        if(connackSent)
            return;
        connackSent = true;
        BinaryEncoder response;
        response.encodeByte(sessionPresent == SessionPresent::Yes ? 1 : 0);
        response.encodeByte(0); // everything okay
        if(req.client->getMQTTVersion() == MQTTVersion::V5) {
            PropertyList properties;
            properties.emplace(MQTTProperty::TOPIC_ALIAS_MAXIMUM, uint16_t(0));
            properties.emplace(MQTTProperty::SUBSCRIPTION_IDENTIFIER_AVAILABLE, uint8_t(0)); // FIXME support sub identifiers
            properties.emplace(MQTTProperty::SHARED_SUBSCRIPTION_AVAILABLE, uint8_t(0));
            response.encodePropertyList(properties);
        }
        req.client->sendData(EncodedPacket::fromData(static_cast<uint8_t>(MQTTMessageType::CONNACK) << 4, response.moveData()));
        req.client->setState(MQTTClientConnection::ConnectionState::CONNECTED);
    };

    if(existingSession != mPersistentClientStates.end()) {
        // disconnect existing client
        auto [existingClient, existingClientLock] = existingSession->second->getCurrentClient();
        if(req.cleanSession == CleanSession::Yes || existingSession->second->isCleanSession() == CleanSession::Yes) {
            existingSession->second->replaceCurrentClient(existingClientLock, req.client, req.cleanSession, PersistentClientState::ReplaceStyle::CleanSession);
            if(existingClient) {
                spdlog::warn("[{}] Already logged in, closing old connection", req.clientId);
                logoutClient(*existingClient);
            }
        } else {
            sessionPresent = SessionPresent::Yes;
            existingSession->second->replaceCurrentClient(existingClientLock, req.client, req.cleanSession, PersistentClientState::ReplaceStyle::SimpleReplace);
            if(existingClient) {
                spdlog::warn("[{}] Already logged in, closing old connection", req.clientId);
                logoutClient(*existingClient);
            }
            sendConnack();
            std::vector<HighQoSRetainStorage> orderedPackets;
            orderedPackets.reserve(existingSession->second->getHighQoSSendingPackets().size());
            for(auto& packet: existingSession->second->getHighQoSSendingPackets()) {
                // FIXME check mqtt version & reencode maybe
                orderedPackets.emplace_back(packet.second);
            }
            std::sort(orderedPackets.begin(), orderedPackets.end(), [](auto& a, auto& b) {
                return a.getGlobalOrder() < b.getGlobalOrder();
            });

            for(auto& highQoSPacket : orderedPackets) {
                auto cpy = highQoSPacket.getPacketSharedCopy();
                cpy.setDupFlag();
                // On a sidenote: The dup flag is so useless. Like no MQTT implementation really makes use of it, most just hand it to the client who
                // ignores it. Even we ignore the dup flags for packets we receive. Luckily, we can avoid the expensive buffer copies due to only having
                // to modify the first byte
                req.client->sendData(std::move(cpy));
            }
            for(size_t i = 0; i < existingSession->second->getQoS2PubRecReceived().count(); ++i) {
                if(existingSession->second->getQoS2PubRecReceived()[i]) {
                    BinaryEncoder encoder;
                    encoder.encode2Bytes(i);
                    req.client->sendData(EncodedPacket::fromData((static_cast<uint8_t>(MQTTMessageType::PUBREL) << 4) | 0b10, encoder.moveData()));
                }
            }
        }
    } else {
        // no session exists
        auto newState = mPersistentClientStates.emplace_hint(existingSession, std::piecewise_construct, std::make_tuple(req.clientId), std::make_tuple(std::make_unique<PersistentClientState>(req.clientId, req.cleanSession, req.client)));
        sessionPresent = SessionPresent::No;
        auto lock = newState->second->getLock();
        newState->second->replaceCurrentClient(lock, req.client, req.cleanSession, PersistentClientState::ReplaceStyle::CleanSession);
    }

    // send CONNACK now
    // we need to do it here because only here we now the value of the session present flag
    // we could use callbacks, but that seems too complicated
    spdlog::info("[{}] Logged in from [{}:{}]", req.client->getClientId(), req.client->getTcpClient().getRemoteIp(), req.client->getTcpClient().getRemotePort());
    sendConnack();
}
void ApplicationState::operator()(ChangeRequestLogoutClient&& req) {
    if(req.client->isLoggedOut())
        return;
    logoutClient(*req.client);
}
void ApplicationState::operator()(ChangeRequestAddScript&& req) {
    auto existingScript = mScripts.find(req.name);
    if(existingScript != mScripts.end()) {
        deleteScript(existingScript);
    }
    auto extension = getFileExtension(req.name);
    if(extension == ".js") {
        auto script = std::make_unique<ScriptContainerJS>(*this, req.name, std::move(req.code));
        auto inserted = mScripts.emplace(req.name, std::move(script));
        inserted.first->second->init(std::move(req.statusOutput));
    } else if(extension == ".cpp") {
        mNativeLibManager.enqueue(CompileNativeLibraryData{ std::move(req.statusOutput), req.name, std::move(req.code) });
    } else {
        req.statusOutput.error(req.name, "Invalid script filename: " + req.name);
    }
    if(req.fromStartup) {
        mScriptsAlreadyAddedFromStartup += 1;
    }
}
void ApplicationState::operator()(ChangeRequestDeleteScript&& req) {
    deleteScript(mScripts.find(req.name));
    SQLite::Statement deleteQuery{ mDb, "DELETE FROM script WHERE name=?" };
    deleteQuery.bind(1, req.name);
    deleteQuery.exec();
}
void ApplicationState::operator()(ChangeRequestActivateScript&& req) {
    auto script = mScripts.find(req.name);
    if(script == mScripts.end())
        return;
    SQLite::Statement updateQuery{ mDb, "UPDATE script SET active=true WHERE name=?" };
    updateQuery.bind(1, req.name);
    updateQuery.exec();
    script->second->activate();
}
void ApplicationState::operator()(ChangeRequestDeactivateScript&& req) {
    auto script = mScripts.find(req.name);
    if(script == mScripts.end())
        return;
    SQLite::Statement updateQuery{ mDb, "UPDATE script SET active=false WHERE name=?" };
    updateQuery.bind(1, req.name);
    updateQuery.exec();
    script->second->deactivate();
}

void ApplicationState::deleteScript(std::unordered_map<std::string, std::unique_ptr<ScriptContainer>>::iterator it) {
    if(it == mScripts.end())
        return;
    it->second->forceQuit();
    deleteAllSubscriptions(*it->second);
    mScripts.erase(it);
}
void ApplicationState::logoutClient(MQTTClientConnection& client) {
    if(client.isLoggedOut())
        return;
    mShouldCleanup = true;
    mClientManager.removeClientConnection(client);
    client.notifyLoggedOut();

    // perform will
    auto willMsg = client.moveWill();
    if(willMsg) {
        publishInternal(std::move(willMsg->topic), std::move(willMsg->payload), willMsg->qos, willMsg->retain, willMsg->properties);
    }

    spdlog::info("[{}] Logged out", client.getClientId());
    // detach persistent state
    auto state = client.getPersistentClientState();
    if(state) {
        state->dropCurrentClient();
        if(state->isCleanSession() == CleanSession::Yes) {
            auto actualState = std::find_if(mPersistentClientStates.begin(), mPersistentClientStates.end(), [&](const auto& upState) {
                return upState.second.get() == state;
            });
            if(actualState == mPersistentClientStates.end()) {
                return;
            }
            mSubscriptions.removeAllSubscriptions(Subscription{state, QoS::QoS0}); // QoS doesn't matter here
            mDeletedPersistentClientStates.emplace_back(std::move(actualState->second));
            mPersistentClientStates.erase(actualState);
            mShouldCleanup = true;
        }
    }
}
void ApplicationState::publish(std::string&& topic, std::vector<uint8_t>&& msg, QoS qos, Retain retain, const PropertyList& properties) {
    std::shared_lock<std::shared_mutex> lock{ mMutex };
    retain = publishNoLockNoRetain(topic, msg, qos, retain, properties);
    lock.unlock();
    if(retain == Retain::Yes) {
        // we aren't allowed to call requestChange from another thread while holding a lock, so we need to do it here
        requestChange(ChangeRequestRetain{ MQTTPacket{std::move(topic), std::move(msg), qos, Retain::Yes, properties} });
    }
}
void ApplicationState::publishInternal(std::string&& topic, std::vector<uint8_t>&& msg, QoS qos, Retain retain, const PropertyList& properties) {
    retain = publishNoLockNoRetain(topic, msg, qos, retain, properties);
    if(retain == Retain::Yes) {
        requestChange(ChangeRequestRetain{ MQTTPacket{std::move(topic), std::move(msg), qos, Retain::Yes, properties} });
    }
}
Retain ApplicationState::publishNoLockNoRetain(const std::string& topic, const std::vector<uint8_t>& msg, QoS publishQoS, Retain retain, const PropertyList& properties) {
// NOTE: It's possible that we only have a read-only lock here, so we aren't allowed to call requestChange
#ifndef NDEBUG
    if(topic != LOG_TOPIC) {
        std::string dataAsStr{ msg.begin(), msg.end() };
        spdlog::trace("Publishing on '{}' data '{}'", topic, dataAsStr);
    }
#endif
    // first check for publish to $NIOEV
    if(startsWith(topic, "$NIOEV")) {
        performSystemAction(topic, std::string_view{(const char*)msg.data(), (const char*)msg.data() + msg.size()}, msg);
        //retain = Retain::No;
    }
    std::unordered_set<Subscriber*> subs;

    mSubscriptions.forEveryMatch(topic, [&topic, &msg, publishQoS, &subs, &properties](Subscription& sub) {
        if(subs.contains(sub.subscriber))
            return;
        subs.emplace(sub.subscriber);
        // according to the spec, we have to downgrade the publishQoS level here to match that of the publish; TODO allow overriding this behaviour in a config file
        auto usedQos = minQoS(sub.qos, publishQoS);
        sendPublish(*sub.subscriber, topic, msg, usedQos, Retained::No, properties);
    });
    return retain;
    // TODO reimplement sync scripts
}
void ApplicationState::handleNewClientConnection(TcpClientConnection&& conn) {
    UniqueLockWithAtomicTidUpdate<std::shared_mutex> lock{ mMutex, mCurrentRWHolderOfMMutex };
    spdlog::info("New connection from [{}:{}]", conn.getRemoteIp(), conn.getRemotePort());
    auto& newClient = mClients.emplace_back(*this, std::move(conn));
    mClientManager.addClientConnection(newClient);
}
void ApplicationState::deleteAllSubscriptions(Subscriber& sub) {
    mSubscriptions.removeAllSubscriptions(Subscription{&sub, QoS::QoS0}); // QoS doesn't matter here
}
ApplicationState::ScriptsInfo ApplicationState::getScriptsInfo() {
    UniqueLockWithAtomicTidUpdate lock{ mMutex, mCurrentRWHolderOfMMutex };
    ScriptsInfo ret;
    SQLite::Statement scriptQuery(mDb, "SELECT name,code FROM script ORDER BY name ASC");
    while(scriptQuery.executeStep()) {
        ScriptsInfo::ScriptInfo scriptInfo;
        scriptInfo.name = scriptQuery.getColumn(0).getString();
        scriptInfo.code = scriptQuery.getColumn(1).getString();
        auto script = mScripts.find(scriptInfo.name);
        if(script != mScripts.end()) {
            scriptInfo.active = script->second->getState();
        }
        ret.scripts.emplace_back(std::move(scriptInfo));
    }
    return ret;
}
void ApplicationState::syncRetainedMessagesToDb() {
    UniqueLockWithAtomicTidUpdate lock{ mMutex, mCurrentRWHolderOfMMutex };
    SQLite::Transaction transaction{ mDb };
    mDb.exec("DELETE FROM retained_msg");
    for(auto& msg : mRetainedMessages) {
        mQueryInsertRetainedMsg->bindNoCopy(1, msg.first);
        mQueryInsertRetainedMsg->bindNoCopy(2, msg.second.payload.data(), msg.second.payload.size());
        struct tm res;
        gmtime_r(&msg.second.timestamp, &res);
        std::stringstream timestampAsStr;
        timestampAsStr << std::put_time(&res, "%Y-%m-%d %H-%M-%S.000");
        mQueryInsertRetainedMsg->bind(3, timestampAsStr.str());
        mQueryInsertRetainedMsg->bind(4, static_cast<int>(msg.second.qos));

        mQueryInsertRetainedMsg->exec();
        mQueryInsertRetainedMsg->clearBindings();
        mQueryInsertRetainedMsg->reset();
    }
    transaction.commit();
    mDb.exec("VACUUM");
    spdlog::info("Synced retained messages to db");
}
void ApplicationState::addScript(
    std::string name, std::function<void(const std::string&, const std::string&)>&& onSuccess, std::function<void(const std::string&, const std::string&)>&& onError, std::string code) {
    if(!hasValidScriptExtension(name))
        return;
    UniqueLockWithAtomicTidUpdate<std::shared_mutex> lock{ mMutex, mCurrentRWHolderOfMMutex };
    mQueryInsertScript->bindNoCopy(1, name);
    mQueryInsertScript->bindNoCopy(2, code);
    mQueryInsertScript->exec();
    mQueryInsertScript->reset();
    mQueryInsertScript->clearBindings();
    ScriptStatusOutput statusOutput;
    statusOutput.success = std::move(onSuccess);
    auto originalError = std::move(statusOutput.error);
    statusOutput.error = [onError = std::move(onError), originalError = std::move(originalError)](auto& scriptName, const auto& error) {
        originalError(scriptName, error);
        onError(scriptName, error);
    };
    requestChange(ChangeRequestAddScript{ std::move(name), std::move(code), std::move(statusOutput) });
}
std::unordered_map<std::string, uint64_t> ApplicationState::getSubscriptionsCount() {
    std::shared_lock<std::shared_mutex> lock{ mMutex };
    std::unordered_map<std::string, uint64_t> counts;
    auto addSub = [&](const Subscriber& sub) {
        auto it = counts.find(sub.getType());
        if(it == counts.end()) {
            counts.emplace(sub.getType(), 1);
        } else {
            it->second += 1;
        }
    };
    /*for(auto& s : mSimpleSubscriptions) {
        addSub(*s.second.subscriber);
    }
    for(auto& s : mWildcardSubscriptions) {
        addSub(*s.subscriber);
    }
    for(auto& s : mOmniSubscriptions) {
        addSub(*s.subscriber);
    }*/
    // FIXME readd
    return counts;
}
void ApplicationState::runScript(const std::string& name, const ScriptInputArgs& input, ScriptStatusOutput&& output) {
    while(true) {
        std::shared_lock<std::shared_mutex> lock{ mMutex };
        auto script = mScripts.find(name);
        if(script == mScripts.end()) {
            if(mAmountOfScriptsLoadedFromDBOnStartup <= mScriptsAlreadyAddedFromStartup) {
                output.error(name, "Couldn't find script");
                return;
            }
            lock.unlock();
            spdlog::warn("Stalling foreign script call while waiting for the script to appear (this only happens during startup)");
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if(!script->second->isRunning()) {
            output.error(name, "Script isn't running!");
            return;
        }
        mScripts.at(name)->run(input, std::move(output));
        return;
    }
}
void ApplicationState::performSystemAction(const std::string& topic, std::string_view payloadStr, const std::vector<uint8_t>& payloadBytes) {
    if(payloadStr.size() > 1024 * 1024) {
        spdlog::warn("Dropping system action request due to 1MB size limit");
        return;
    }
    if(topic == "$NIOEV/request_new_stats") {
        assert(mStatistics);
        mStatistics->refresh();
    }
}

void PersistentClientState::publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained, const PropertyList& properties, MQTTPublishPacketBuilder& packetBuilder) {
    std::unique_lock<std::recursive_mutex> lock{mMutex};
    if(qos == QoS::QoS0) {
        mCurrentClient->publish(topic, payload, QoS::QoS0, retained, properties, packetBuilder, 0);
        return;
    }
    MQTTVersion encoderVersion = MQTTVersion::V4;
    if(mCurrentClient) {
        encoderVersion = mCurrentClient->getMQTTVersion();
    }
    // TODO smarter packet id choosing
    uint16_t packetId;
    do {
        mPacketIdCounter += 1;
    } while(mPacketIdCounter == 0);
    packetId = mPacketIdCounter;

    if(qos == QoS::QoS1) {
        mHighQoSSendingPackets.emplace(packetId, HighQoSRetainStorage{packetBuilder.getPacket(qos, packetId, encoderVersion), qos, encoderVersion});
    } else if(qos == QoS::QoS2) {
        mHighQoSSendingPackets.emplace(packetId, HighQoSRetainStorage{packetBuilder.getPacket(qos, packetId, encoderVersion), qos, encoderVersion});
    }
    if(mCurrentClient) {
        // FIXME release lock somehow here???
        mCurrentClient->publish(topic, payload, qos, retained, properties, packetBuilder, packetId);
    }
}
}