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

struct ScriptRunArgsTcpNewClient {
    int fd;
};

struct ScriptRunArgsTcpNewDataFromClient {
    int fd;
    std::vector<uint8_t> data;
};

struct ScriptRunArgsTcpDeleteClient {
    int fd;
};

struct ScriptRunArgsInterval {

};

using ScriptInputArgs = std::variant<ScriptRunArgsMqttMessage, ScriptRunArgsTcpNewClient, ScriptRunArgsTcpNewDataFromClient, ScriptRunArgsTcpDeleteClient, ScriptRunArgsInterval>;

enum class SyncAction {
    Continue,
    AbortPublish
};

struct ScriptStatusOutput {
    std::function<void(const std::string& scriptName)> success = [](auto&) {};
    std::function<void(const std::string& scriptName, const std::string& reason)> error = [](auto& scriptName, const auto& error) {spdlog::error("[{}] Script error: {}", scriptName, error);};
    std::function<void(const std::string& scriptName, SyncAction action)> syncAction = [](auto&, auto) {};
};

class ScriptContainer : public Subscriber {
public:
    explicit ScriptContainer(ApplicationState& p, std::string code)
    : mApp(p), mCode(std::move(code)) {

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

    void publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained) override {
        run(ScriptRunArgsMqttMessage{topic, payload, retained}, ScriptStatusOutput{});
    }
    const auto& getCode() const {
        return mCode;
    }

protected:
    mutable std::mutex mScriptInitReturnMutex;
    std::optional<ScriptInitReturn> mScriptInitReturn;
    ApplicationState& mApp;
    std::string mCode;
};

}