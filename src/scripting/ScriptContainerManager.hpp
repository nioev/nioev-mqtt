#pragma once

#include "ScriptContainer.hpp"

#include <memory>
#include <spdlog/spdlog.h>
#include <unordered_map>


namespace nioev {

class ScriptContainerManager {
public:
    template<typename T, typename... Args>
    void addScript(const std::string& name, ScriptStatusOutput&& statusOutput, ScriptActionPerformer& actionPerformer, Args&&... args) {
        auto scriptPtr = new T(actionPerformer, name, std::forward<Args>(args)...);
        auto success = std::move(statusOutput.success);
        statusOutput.success = [this, success, statusOutput, name, scriptPtr] (auto& scriptName) {
            mScripts.emplace(std::string{name}, std::unique_ptr<T>{scriptPtr});
            success(scriptName);
        };
        scriptPtr->init(std::move(std::move(statusOutput)));
    }

    void deleteScript(const std::string& name) {
        auto script = mScripts.find(name);
        if(script == mScripts.end())
            return;
        script->second->forceQuit();
        mScripts.erase(script);
    }

    [[nodiscard]] const ScriptInitReturn& getScriptInitReturn(const std::string& name) const {
        return mScripts.at(name)->getInitArgs();
    }

    void runScript(const std::string& name, const ScriptInputArgs& in, ScriptStatusOutput&& out) {
        auto& script = mScripts.at(name);
        script->run(in, std::move(out));
    }

private:
    std::unordered_map<std::string, std::unique_ptr<ScriptContainer>> mScripts;
};

}