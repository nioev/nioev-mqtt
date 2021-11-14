#pragma once

#include "ScriptContainer.hpp"

#include <memory>
#include <spdlog/spdlog.h>
#include <unordered_map>


namespace nioev {

class ScriptContainerManager {
public:
    template<typename T, typename... Args>
    void addScript(const std::string& name, const ScriptInitOutputArgs& outputArgs, Args... args) {
        auto scriptPtr = new T(name, std::forward<Args>(args)...);
        auto outputArgsCpy = outputArgs;
        outputArgsCpy.success = [this, outputArgs, name, scriptPtr] (const auto& initRet) {
            mScripts.emplace(std::string{name}, std::unique_ptr<T>{scriptPtr});
            outputArgs.success(initRet);
        };
        scriptPtr->init(std::move(outputArgsCpy));
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

    void runScript(const std::string& name, const ScriptInputArgs& in, const ScriptOutputArgs& out) {
        auto& script = mScripts.at(name);
        script->run(in, out);
    }

private:
    std::unordered_map<std::string, std::unique_ptr<ScriptContainer>> mScripts;
};

}