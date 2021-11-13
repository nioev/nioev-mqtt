#pragma once

#include "ClientThreadManager.hpp"
#include "MQTTClientConnection.hpp"
#include "MQTTPersistentState.hpp"
#include "scripting/ScriptContainerManager.hpp"
#include "TcpClientHandlerInterface.hpp"
#include <thread>
#include <unordered_map>
#include <vector>

namespace nioev {

class Application : public TcpClientHandlerInterface {
private:
    std::unordered_map<int, MQTTClientConnection> mClients;
    ClientThreadManager mClientManager;
    MQTTPersistentState mPersistentState;
    ScriptContainerManager mScripts;
    std::shared_mutex mClientsMutex;

    void publishWithoutAcquiringLock(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain);
    ScriptOutputArgs getDefaultScriptOutputArgs(const std::string& scriptName);

    SyncAction runScriptWithPublishedMessage(const std::string& scriptName, const std::string& topic, const std::vector<uint8_t>& payload, Retained retained);

public:
    Application();
    void handleNewClientConnection(TcpClientConnection&&) override;
    std::pair<std::reference_wrapper<MQTTClientConnection>, std::shared_lock<std::shared_mutex>> getClient(int fd);
    void notifyConnectionError(int connFd);
    void publish(std::string&& topic, std::vector<uint8_t>&& msg, std::optional<QoS> qos, Retain retain);
    void addSubscription(MQTTClientConnection& conn, std::string&& topic, QoS qos);
    void deleteSubscription(MQTTClientConnection& conn, const std::string& topic);

    template<typename T, typename... Args>
    void addScript(const std::string& name, const std::function<void()>& onSuccess, const std::function<void(const std::string&)>& onError, Args... args) {
        auto scriptOutput = getDefaultScriptOutputArgs(name);
        scriptOutput.error = onError;
        ScriptInitOutputArgs scriptInitOutput{ .error = onError,
                                           .success = [onSuccess](const auto& initArgs) { onSuccess(); },
                                           .initialActionsOutput = scriptOutput};
        mScripts.addScript<T>(name, scriptInitOutput, std::forward<Args>(args)...);
    }
    void deleteScript(const std::string& name) {
        mScripts.deleteScript(name);
    }
};

}