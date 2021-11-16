#include "Application.hpp"
#include <condition_variable>
#include <spdlog/spdlog.h>

namespace nioev {

Application::Application()
: mClientManager(*this, 4), mScriptActionPerformer(*this) {

}

void Application::handleNewClientConnection(TcpClientConnection&& conn) {
    spdlog::info("New Client {}:{}", conn.getRemoteIp(), conn.getRemotePort());
    std::lock_guard<std::shared_mutex> lock{mClientsMutex};
    int fd = conn.getFd();
    auto newClient = mClients.emplace(
        std::piecewise_construct,
        std::make_tuple(fd),
        std::make_tuple(std::move(conn)));
    mClientManager.addClientConnection(newClient.first->second);
}
std::pair<std::reference_wrapper<MQTTClientConnection>, std::shared_lock<std::shared_mutex>> Application::getClient(int fd) {
    std::shared_lock<std::shared_mutex> lock{mClientsMutex};
    return {mClients.at(fd), std::move(lock)};
}
void Application::notifyConnectionError(int connFd) {
    std::lock_guard<std::shared_mutex> lock{mClientsMutex};
    auto client = mClients.find(connFd);
    if(client == mClients.end()) {
        // Client was already deleted. This can happen if two receiver threads
        // get notified at the same time that a connection was closed and
        // both try to delete the connection at the same time.
        return;
    }
    spdlog::debug("Deleting connection {}", connFd);
    auto willMsg = client->second.moveWill();
    if(willMsg) {
        publishWithoutAcquiringLock(std::move(willMsg->topic), std::move(willMsg->msg), willMsg->qos, willMsg->retain);
    }
    mClientManager.removeClientConnection(client->second);
    mPersistentState.deleteAllSubscriptions(client->second);

    mClients.erase(client);
}
void Application::publish(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain) {
    std::shared_lock<std::shared_mutex> lock{mClientsMutex};
    publishWithoutAcquiringLock(std::move(topic), std::move(msg), qos, retain);
}
void Application::publishWithoutAcquiringLock(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain) {
#ifndef NDEBUG
    {
        std::string dataAsStr{msg.begin(), msg.end()};
        spdlog::info("Publishing on '{}' data '{}'", topic, dataAsStr);
    }
#endif
    // first run scripts
    auto action = SyncAction::Continue;
    mPersistentState.forEachSubscriber(topic, [this, &topic, &msg, &action] (auto& sub) {
        if(sub.subscriber.index() != 1)
            return;
        if(runScriptWithPublishedMessage(std::get<MQTTPersistentState::ScriptName>(sub.subscriber), topic, msg, Retained::No) == SyncAction::AbortPublish) {
            action = SyncAction::AbortPublish;
        }
    });
    if(action == SyncAction::AbortPublish) {
        return;
    }
    // then send to clients
    // this order is neccessary to allow the scripts to abort the message delivery to clients
    mPersistentState.forEachSubscriber(topic, [this, &topic, &msg] (auto& sub) {
        if(sub.subscriber.index() != 0)
            return;
        assert(sub.qos);
        mClientManager.sendPublish(std::get<std::reference_wrapper<MQTTClientConnection>>(sub.subscriber), topic, msg, *sub.qos, Retained::No);
    });
    if(retain == Retain::Yes) {
        mPersistentState.retainMessage(std::move(topic), std::move(msg));
    }
}
void Application::addSubscription(MQTTClientConnection& conn, std::string&& topic, QoS qos) {
    std::shared_lock<std::shared_mutex> lock{mClientsMutex};
    mPersistentState.addSubscription(conn, std::move(topic), qos, [&](const auto& topic, const auto& payload) {
        // this callback gets called for each retained message that we now need to publish
        mClientManager.sendPublish(conn, topic, payload, qos, Retained::Yes);
    });
}
void Application::addSubscription(std::string&& scriptName, std::string&& topic) {
    mPersistentState.addSubscription(scriptName, std::move(topic), [this, &scriptName](const auto& recvTopic, const auto& recvPayload) {
        runScriptWithPublishedMessage(scriptName, recvTopic, recvPayload, Retained::Yes);
    });
}
void Application::deleteSubscription(MQTTClientConnection& conn, const std::string& topic) {
    mPersistentState.deleteSubscription(conn, topic);
}
void Application::deleteSubscription(std::string&& scriptName, std::string&& topic) {
    mPersistentState.deleteSubscription(std::move(scriptName), topic);
}
SyncAction Application::runScriptWithPublishedMessage(const std::string& scriptName, const std::string& topic, const std::vector<uint8_t>& payload, Retained retained) {
    std::atomic<SyncAction> ret = SyncAction::Continue;

    if(mScripts.getScriptInitReturn(scriptName).runType == ScriptRunType::Sync) {
        std::condition_variable cv;
        std::mutex m;

        ScriptStatusOutput statusOutput;
        bool done = false;
        statusOutput.syncAction = [&](auto&, auto syncAction) {
            ret = syncAction;
        };
        statusOutput.error = [&](auto&, auto& msg) {
            std::unique_lock<std::mutex> lock{m};
            done = true;
            lock.unlock();
            cv.notify_all();
        };
        statusOutput.success = [&](auto&) {
            std::unique_lock<std::mutex> lock{m};
            done = true;
            lock.unlock();
            cv.notify_all();
        };
        mScripts.runScript(scriptName, ScriptRunArgsMqttMessage{topic, payload, retained}, std::move(statusOutput));
        std::unique_lock<std::mutex> l{m};
        while(!done) {
            cv.wait(l);
        }
        return ret;
    } else {
        ScriptStatusOutput statusOutput;
        statusOutput.error = [](auto& name, auto& msg) {
            spdlog::warn("Script '{}' failed with '{}'", name, msg);
        };
        mScripts.runScript(scriptName, ScriptRunArgsMqttMessage{topic, payload, retained}, std::move(statusOutput));
        return SyncAction::Continue;
    }
}
}