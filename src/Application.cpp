#include "Application.hpp"
#include <spdlog/spdlog.h>

namespace nioev {

Application::Application()
: mClientManager(*this, 4) {

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
    mPersistentState.forEachSubscriber(topic, [this, &topic, &msg] (auto& sub) {
        mClientManager.sendPublish(sub.conn, topic, msg, sub.qos, Retained::No);
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
void Application::deleteSubscription(MQTTClientConnection& conn, const std::string& topic) {
    mPersistentState.deleteSubscription(conn, topic);
}
ScriptOutputArgs Application::getScriptOutputArgs(std::function<void()> onSuccess, std::function<void(const std::string&)> onError) {
    return ScriptOutputArgs{
        .publish = [this](auto&& topic, auto&& payload, auto qos, auto retain) {
            publish(std::move(topic), std::move(payload), qos, retain);
        },
        .subscribe = [this](const auto&) {

        },
        .unsubscribe = [this](const auto&) {

        },
        .error = std::move(onError),
        .syncAction = [](auto) {

        },
        .success = std::move(onSuccess)
    };
}
}