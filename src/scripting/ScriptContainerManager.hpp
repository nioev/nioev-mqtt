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
        auto scriptPtr = std::make_unique<T>(name, std::forward<Args>(args)...);
        auto script = mScripts.emplace(std::string{name}, std::move(scriptPtr));
        script.first->second->init(outputArgs);
    }

    void deleteScript(const std::string& name) {
        auto script = mScripts.find(name);
        if(script == mScripts.end())
            return;
        script->second->forceQuit();
        mScripts.erase(script);
    }

    void runScript(const std::string& name, const ScriptInputArgs& in, const ScriptOutputArgs& out) {
        mScripts.at(name)->run(in, out);
    }

private:
    std::unordered_map<std::string, std::unique_ptr<ScriptContainer>> mScripts;
};

}