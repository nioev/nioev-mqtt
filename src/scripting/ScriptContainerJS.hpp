#pragma once
#include "ScriptContainer.hpp"
#include <atomic>
#include "quickjs.h"
#include <thread>
#include <optional>
#include <queue>
#include <condition_variable>
#include "NativeLibrary.hpp"

namespace nioev {

class ScriptContainerJS final : public ScriptContainer {
public:
    ScriptContainerJS(ApplicationState& p, const std::string& scriptName, std::string&& scriptCode);
    ~ScriptContainerJS();
    void init(ScriptStatusOutput&&) override;
    void run(const ScriptInputArgs&, ScriptStatusOutput&&) override;
    void forceQuit() override;
private:
    void scriptThreadFunc(ScriptStatusOutput&&);
    void performRun(const ScriptInputArgs&, ScriptStatusOutput&&);
    std::string getJSException();
    void handleScriptActions(const JSValue& actions, ScriptStatusOutput&& status);
    std::optional<std::string> getJSStringProperty(const JSValue& obj, std::string_view name);
    std::optional<int> getJSIntProperty(const JSValue& obj, std::string_view name);
    std::optional<bool> getJSBoolProperty(const JSValue& obj, std::string_view name);
    std::optional<std::vector<uint8_t>> extractPayload(const JSValue& obj);

    std::atomic<bool> mShouldAbort = false;
    JSRuntime* mJSRuntime;
    JSContext* mJSContext;
    std::optional<std::thread> mScriptThread;

    std::string mName;

    std::mutex mTasksMutex;
    std::condition_variable mTasksCV;
    std::queue<std::pair<ScriptInputArgs, ScriptStatusOutput>> mTasks;
    std::string mInitFailureMessage; // protected by tasks mutex

    struct IntervalData {
        std::chrono::steady_clock::time_point mLastIntervalCall{std::chrono::steady_clock::now()};
        std::chrono::milliseconds mIntervalTimeout{0};
    };
    std::optional<IntervalData> mIntervalData;
    std::unordered_map<std::string, NativeLibrary> mNativeLibs;
};

}
