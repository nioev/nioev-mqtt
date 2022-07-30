#pragma once

#include "ScriptContainer.hpp"

#include <memory>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <shared_mutex>


namespace nioev::mqtt {

class ScriptContainerManager {
public:
    ScriptContainerManager(ApplicationState& app)
    : mApp(app) {

    }

    template<typename T, typename... Args>
    void addScript(const std::string& name, ScriptStatusOutput&& statusOutput, ScriptActionPerformer& actionPerformer, Args&&... args) {
        std::unique_lock<std::shared_mutex> lock{mScriptsLock};

        auto existingScript = mScripts.find(name);
        while(existingScript != mScripts.end()) {
            spdlog::info("Replacing script {}", name);
            // We need to go through the app so that the App can delete the script's subscriptions.
            // The app then calls deleteScript from us, which acquires our lock, which is why we need to drop the lock here
            // to avoid a deadlock. Afterwards in the short duration until we reacquire the lock, it's possible our references got invalidated,
            // so we need to update it and check again until we can be sure that we have the rw lock and there is no existing script.
            lock.unlock();
            deleteScriptFromApp(name);
            lock.lock();
            existingScript = mScripts.find(name);
        }

        statusOutput.error = [out = std::move(statusOutput.error)](auto& scriptName, auto& reason) {
            spdlog::error("Script init [{}] error: {}", scriptName, reason);
            out(scriptName, reason);
        };

        auto[script, insertionSuccessful] = mScripts.emplace(std::string{name}, std::make_unique<T>(actionPerformer, name, std::forward<Args>(args)...));
        script->second->init(std::move(statusOutput));
    }

    void deleteScript(const std::string& name) {
        std::unique_lock<std::shared_mutex> lock{mScriptsLock};
        auto script = mScripts.find(name);
        if(script == mScripts.end())
            return;
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
        out.error = [out = std::move(out.error)](auto& scriptName, auto& reason) {
            spdlog::error("Script run [{}] error: {}", scriptName, reason);
            out(scriptName, reason);
        };
        script->run(in, std::move(out));
    }
private:
    void deleteScriptFromApp(const std::string& name);
    mutable std::shared_mutex mScriptsLock;
    std::unordered_map<std::string, std::unique_ptr<ScriptContainer>> mScripts;

    ApplicationState& mApp;
};

}