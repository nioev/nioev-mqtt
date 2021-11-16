#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <variant>
#include <functional>
#include "../Enums.hpp"
#include "../Forward.hpp"

namespace nioev {

enum class ScriptRunType {
    Sync,
    Async
};

struct ScriptInitReturn {
    ScriptRunType runType = ScriptRunType::Async;
};

struct ScriptRunArgsMqttMessage {
    std::string topic;
    std::vector<uint8_t> payload;
    Retained retained;
};
using ScriptInputArgs = std::variant<ScriptRunArgsMqttMessage>;

enum class SyncAction {
    Continue,
    AbortPublish
};

struct ScriptStatusOutput {
    std::function<void(const std::string& scriptName, const std::string& reason)> error = [](auto&, auto&) {};
    std::function<void(const std::string& scriptName)> success = [](auto&) {};
    std::function<void(const std::string& scriptName, SyncAction action)> syncAction = [](auto&, auto) {};
};

class ScriptContainer {
public:
    explicit ScriptContainer(ScriptActionPerformer& p)
    : mActionPerformer(p) {

    }
    virtual ~ScriptContainer() = default;
    virtual void init(ScriptStatusOutput&&) = 0;
    virtual void run(const ScriptInputArgs&, ScriptStatusOutput&&) = 0;
    [[nodiscard]] const ScriptInitReturn& getInitArgs() const {
        return mScriptInitReturn;
    }
    virtual void forceQuit() = 0;

protected:
    ScriptInitReturn mScriptInitReturn;
    ScriptActionPerformer& mActionPerformer;
};

}