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
    const std::string& getName() const {
        return mName;
    }
private:
    void scriptThreadFunc(ScriptStatusOutput&&);
    void performRun(const ScriptInputArgs&, ScriptStatusOutput&&);
    std::string getJSException();
    std::optional<std::string> getJSStringProperty(const JSValue& obj, std::string_view name);
    std::optional<int> getJSIntProperty(const JSValue& obj, std::string_view name);
    std::optional<bool> getJSBoolProperty(const JSValue& obj, std::string_view name);
    std::optional<std::vector<uint8_t>> extractPayload(const JSValue& obj);

    std::atomic<bool> mShouldAbort = false;
    JSRuntime* mJSRuntime;
    JSContext* mJSContext;
    std::optional<std::thread> mScriptThread;

    std::string mName;

    std::queue<std::pair<ScriptInputArgs, ScriptStatusOutput>> mTasks;
    std::string mInitFailureMessage; // protected by tasks mutex

    struct TimeoutData {
        std::chrono::steady_clock::time_point mLastIntervalCall{std::chrono::steady_clock::now()};
        std::chrono::milliseconds mIntervalTimeout{0};
        int32_t mId{0};
        mutable JSValue mFunction{JS_UNDEFINED};
        mutable std::vector<JSValue> mFunctionArgs;
        TimeoutData(std::chrono::milliseconds timeout, int32_t id, JSValue function, std::vector<JSValue> functionArgs) {
            mIntervalTimeout = timeout;
            mId = id;
            mFunction = function;
            mFunctionArgs = std::move(functionArgs);
        }
        [[nodiscard]] auto timeLeftUntilShouldBeRun() const {
            return mIntervalTimeout - (std::chrono::steady_clock::now() - mLastIntervalCall);
        }
        bool operator<(const TimeoutData& other) const {
            return timeLeftUntilShouldBeRun() > other.timeLeftUntilShouldBeRun();
        }
        void freeJSValues(JSContext* ctx) const {
            JS_FreeValue(ctx, mFunction);
            for(auto& arg: mFunctionArgs) {
                JS_FreeValue(ctx, arg);
            }
            mFunction = JS_UNDEFINED;
            mFunctionArgs.clear();
        }
    };
    std::unordered_map<std::string, NativeLibrary> mNativeLibs;
    std::priority_queue<TimeoutData> mTimeouts;
    int32_t mTimeoutIdCounter = 1;

    template<bool error>
    friend JSValue jsLog(JSContext* ctx, JSValue this_obj, int argc, JSValue* args);
    template<bool sub>
    friend JSValue jsSubOrUnsub(JSContext* ctx, JSValue this_obj, int argc, JSValue* args);
};

}
