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
    ScriptContainerJS(const std::string& scriptName, std::string&& scriptCode);
    ~ScriptContainerJS();
    void init(const ScriptInitOutputArgs&) override;
    void run(const ScriptInputArgs&, const ScriptOutputArgs&) override;
    void forceQuit() override;
private:
    void scriptThreadFunc(const ScriptInitOutputArgs&);

    std::atomic<bool> mShouldAbort = false;
    JSRuntime* mJSRuntime;
    JSContext* mJSContext;
    std::optional<std::thread> mScriptThread;

    std::string mName;
    std::string mCode;

    std::mutex mTasksMutex;
    std::condition_variable mTasksCV;
    std::queue<std::pair<ScriptInputArgs, ScriptOutputArgs>> mTasks;
};

}
