#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <variant>
#include <functional>
#include <optional>
#include <mutex>
#include "../Enums.hpp"
#include "../Forward.hpp"
#include "spdlog/spdlog.h"
#include "../Subscriber.hpp"
#include <condition_variable>

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

struct ScriptForeignCall {
    std::string payload;
};

struct ScriptInputArgs {
    std::variant<ScriptRunArgsMqttMessage, ScriptForeignCall> params;
    int32_t taskId{-1}; // optionally setable to abort the task
};

enum class SyncAction {
    Continue,
    AbortPublish
};

struct ScriptStatusOutput {
    std::function<void(const std::string& scriptName, const std::string& returnValue)> success = [](auto&, auto&) {};
    std::function<void(const std::string& scriptName, const std::string& reason)> error = [](auto& scriptName, const auto& error) {spdlog::error("[{}] Script error: {}", scriptName, error);};
    std::function<void(const std::string& scriptName, SyncAction action)> syncAction = [](auto&, auto) {};
};

enum class ScriptState {
    UNKNOWN,
    RUNNING,
    STOPPING,
    DEACTIVATED,
    INIT_FAILED
};


static inline const char* scriptStateToString(ScriptState state) {
    const char* stateStr{nullptr};
    switch(state) {
    case ScriptState::RUNNING:
        stateStr = "running";
        break;
    case ScriptState::UNKNOWN:
        stateStr = "unknown";
        break;
    case ScriptState::INIT_FAILED:
        stateStr = "init_failed";
        break;
    case ScriptState::DEACTIVATED:
        stateStr = "deactivated";
        break;
    case ScriptState::STOPPING:
        stateStr = "stopping";
        break;
    default:
        stateStr = "<error>";
    }
    return stateStr;
}

class ScriptContainer : public Subscriber {
public:
    explicit ScriptContainer(ApplicationState& p, std::string name, std::string code)
    : mApp(p), mName(std::move(name)), mCode(std::move(code)) {

    }
    virtual ~ScriptContainer() = default;
    virtual void init(ScriptStatusOutput&&) = 0;
    virtual void run(const ScriptInputArgs&, ScriptStatusOutput&&) = 0;
    [[nodiscard]] std::optional<ScriptInitReturn> getInitArgs() const {
        std::lock_guard<std::mutex> lock{mScriptInitReturnMutex};
        return mScriptInitReturn;
    }
    virtual void forceQuit() = 0;
    auto makeShared() {
        return std::dynamic_pointer_cast<ScriptContainer>(shared_from_this());
    }

    void publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained, MQTTPublishPacketBuilder& packetBuilder) override {
        if(mState != ScriptState::RUNNING)
            return;
        run(ScriptInputArgs{ScriptRunArgsMqttMessage{topic, payload, retained}}, ScriptStatusOutput{});
    }
    const auto& getCode() const {
        return mCode;
    }
    virtual const char* getType() const override {
        return "script";
    }

    /* Deactivated scripts will no longer receive any messages and should not run any further scheduled code.
     */
    void activate() {
        if(mState != ScriptState::DEACTIVATED) {
            return;
        }
        std::unique_lock<std::mutex> lock{mMutex};
        mState = ScriptState::RUNNING;
        mCV.notify_all();
        lock.unlock();
        spdlog::info("[{}] Activated", mName);
    }
    void deactivate() {
        if(mState != ScriptState::RUNNING)
            return;
        mState = ScriptState::DEACTIVATED;
        spdlog::info("[{}] Deactivated", mName);
    }
    [[nodiscard]] bool isActive() const {
        return mState != ScriptState::DEACTIVATED;
    }
    static int32_t allocTaskId() {
        static std::atomic<uint16_t> mTaskID;
        return mTaskID++;
    }
    ScriptState getState() const {
        return mState;
    }

protected:
    mutable std::mutex mMutex;
    mutable std::mutex mScriptInitReturnMutex;
    std::optional<ScriptInitReturn> mScriptInitReturn;
    ApplicationState& mApp;
    std::atomic<ScriptState> mState{ScriptState::RUNNING};
    std::string mCode, mName;
    std::condition_variable mCV;
    std::string mInitFailureMessage; // protected by tasks mutex
};

}