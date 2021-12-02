#pragma once

#include "ScriptContainer.hpp"
#include "ScriptCustomTCPServer.hpp"

#include <memory>
#include <spdlog/spdlog.h>
#include <unordered_map>


namespace nioev {

class ScriptContainerManager {
public:
    ScriptContainerManager()
    : mScriptServer(*this) {

    }

    template<typename T, typename... Args>
    void addScript(const std::string& name, ScriptStatusOutput&& statusOutput, ScriptActionPerformer& actionPerformer, Args&&... args) {
        std::lock_guard<std::shared_mutex> lock{mScriptsLock};
        auto scriptPtr = new T(actionPerformer, name, std::forward<Args>(args)...);
        auto success = std::move(statusOutput.success);
        statusOutput.success = [this, success, statusOutput, name, scriptPtr] (auto& scriptName) {
            mScripts.emplace(std::string{name}, std::unique_ptr<T>{scriptPtr});
            success(scriptName);
        };
        scriptPtr->init(std::move(std::move(statusOutput)));
    }

    void deleteScript(const std::string& name) {
        std::lock_guard<std::shared_mutex> lock{mScriptsLock};
        auto script = mScripts.find(name);
        if(script == mScripts.end())
            return;
        mScriptServer.notifyScriptDied(name);
        script->second->forceQuit();
        mScripts.erase(script);
    }

    [[nodiscard]] std::pair<std::reference_wrapper<const ScriptInitReturn>, std::shared_lock<std::shared_mutex>>
    getScriptInitReturn(const std::string& name) const {
        return {mScripts.at(name)->getInitArgs(), std::shared_lock<std::shared_mutex>{mScriptsLock}};
    }

    void runScript(const std::string& name, const ScriptInputArgs& in, ScriptStatusOutput&& out) {
        std::shared_lock<std::shared_mutex> lock{mScriptsLock};
        auto& script = mScripts.at(name);
        script->run(in, std::move(out));
    }

    void passTcpClient(TcpClientConnection&& tcpClient, std::vector<uint8_t>&& receivedData) {
        mScriptServer.passTcpClient(std::move(tcpClient), std::move(receivedData));
    }
    void scriptTcpListen(std::string&& scriptName, std::string&& listenIdentifier) {
        mScriptServer.scriptListen(std::move(scriptName), std::move(listenIdentifier));
    }
    void scriptTcpSendToClient(std::string&& scriptName, int fd, std::vector<uint8_t>&& payload) {
        mScriptServer.sendMsgFromScript(std::move(scriptName), fd, std::move(payload));
    }
private:
    mutable std::shared_mutex mScriptsLock;
    std::unordered_map<std::string, std::unique_ptr<ScriptContainer>> mScripts;

    ScriptCustomTCPServer mScriptServer;
};

}