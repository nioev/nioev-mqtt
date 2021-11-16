#pragma once
#include "ScriptContainer.hpp"
#include <atomic>
#include "quickjs.h"
#include <thread>
#include <optional>
#include <queue>
#include <condition_variable>

namespace nioev {

class ScriptContainerJS final : public ScriptContainer {
public:
    ScriptContainerJS(ScriptActionPerformer& p, const std::string& scriptName, std::string&& scriptCode);
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

    std::atomic<bool> mShouldAbort = false;
    JSRuntime* mJSRuntime;
    JSContext* mJSContext;
    std::optional<std::thread> mScriptThread;

    std::string mName;
    std::string mCode;

    std::mutex mTasksMutex;
    std::condition_variable mTasksCV;
    std::queue<std::pair<ScriptInputArgs, ScriptStatusOutput>> mTasks;
};

}
