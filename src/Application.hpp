#pragma once

#include "ClientThreadManager.hpp"
#include "MQTTClientConnection.hpp"
#include "MQTTPersistentState.hpp"
#include "scripting/ScriptContainerManager.hpp"
#include "TcpClientHandlerInterface.hpp"
#include "scripting/ScriptActionPerformer.hpp"
#include <thread>
#include <unordered_map>
#include <vector>
#include "Timers.hpp"

namespace nioev {

class Application : public TcpClientHandlerInterface {
private:
    std::unordered_map<int, MQTTClientConnection> mClients;
    MQTTPersistentState mPersistentState;
    ScriptContainerManager mScripts;
    ScriptActionPerformer mScriptActionPerformer;
    std::shared_mutex mClientsMutex;
    Timers mTimer;
    // needs to initialized last because it starts a thread which calls us
    ClientThreadManager mClientManager;

    void publishWithoutAcquiringLock(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain);

    SyncAction runScriptWithPublishedMessage(const std::string& scriptName, const std::string& topic, const std::vector<uint8_t>& payload, Retained retained);
    void performWillWithoutEraseAndLock(MQTTClientConnection& conn);

public:
    Application();
    void handleNewClientConnection(TcpClientConnection&&) override;
    std::pair<std::reference_wrapper<MQTTClientConnection>, std::shared_lock<std::shared_mutex>> getClient(int fd);
    void publish(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain);
    void addSubscription(MQTTClientConnection& conn, std::string&& topic, QoS qos);
    void addSubscription(std::string&& scriptName, std::string&& topic);
    void deleteSubscription(MQTTClientConnection& conn, const std::string& topic);
    void deleteSubscription(std::string&& scriptName, std::string&& topic);
    SessionPresent loginClient(MQTTClientConnection& conn, std::string&& clientId, CleanSession cleanSession);
    void cleanupDisconnectedClients();

    template<typename T, typename... Args>
    void addScript(const std::string& name, std::function<void(const std::string& scriptName)>&& onSuccess, std::function<void(const std::string& scriptName, const std::string&)>&& onError, Args... args) {
        ScriptStatusOutput statusOutput;
        statusOutput.success = std::move(onSuccess);
        statusOutput.error = std::move(onError);
        mScripts.addScript<T>(name, std::move(statusOutput), mScriptActionPerformer, std::forward<Args>(args)...);
    }
    void deleteScript(const std::string& name) {
        mPersistentState.deleteAllSubscriptions(name);
        mScripts.deleteScript(name);
    }
};

}