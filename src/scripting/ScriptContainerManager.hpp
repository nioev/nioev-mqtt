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
        std::unique_lock<std::shared_mutex> lock{mScriptsLock};

        auto existingScript = mScripts.find(name);
        if(existingScript != mScripts.end()) {
            spdlog::info("Replacing script {}", name);
            mScripts.erase(existingScript);
        }

        auto scriptPtr = new T(actionPerformer, name, std::forward<Args>(args)...);
        mScripts.emplace(std::string{name}, std::unique_ptr<T>{scriptPtr});
        scriptPtr->init(std::move(statusOutput));
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

    [[nodiscard]] std::optional<std::pair<std::optional<ScriptInitReturn>, std::shared_lock<std::shared_mutex>>>
    getScriptInitReturn(const std::string& name) const {
        std::shared_lock<std::shared_mutex> lock{mScriptsLock};
        auto script = mScripts.find(name);
        if(script == mScripts.end()) {
            return {};
        }
        return {{script->second->getInitArgs(), std::move(lock)}};
    }

    void runScript(const std::string& name, const ScriptInputArgs& in, ScriptStatusOutput&& out) {
        std::shared_lock<std::shared_mutex> lock{mScriptsLock};
        auto& script = mScripts.at(name);
        script->run(in, std::move(out));
    }

    void passTcpClient(TcpClientConnection&& tcpClient, std::vector<uint8_t>&& receivedData) {
        mScriptServer.passTcpClient(std::move(tcpClient), std::move(receivedData));
    }
    void scriptTcpListen(std::string&& scriptName, std::string&& listenIdentifier, Compression sendCompression, Compression recvCompression) {
        mScriptServer.scriptListen(std::move(scriptName), std::move(listenIdentifier), sendCompression, recvCompression);
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