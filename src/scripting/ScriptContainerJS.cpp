#include "ScriptContainerJS.hpp"
#include "quickjs.h"
#include "../Util.hpp"
#include "spdlog/spdlog.h"

namespace nioev {

ScriptContainerJS::ScriptContainerJS(const std::string& scriptName, std::string&& scriptCode)
: mName(scriptName), mCode(std::move(scriptCode)) {
    mJSRuntime = JS_NewRuntime();
    mJSContext = JS_NewContext(mJSRuntime);
    JS_SetInterruptHandler(
        mJSRuntime,
        [](JSRuntime*, void* scriptContainerJS) -> int {
            auto* self = reinterpret_cast<ScriptContainerJS*>(scriptContainerJS);
            return self->mShouldAbort;
        },
        this);
}
ScriptContainerJS::~ScriptContainerJS() {
    JS_FreeContext(mJSContext);
    mJSContext = nullptr;
    JS_FreeRuntime(mJSRuntime);
    mJSRuntime = nullptr;
}

void ScriptContainerJS::init(const ScriptInitOutputArgs& initOutput) {
    mScriptThread.emplace([this, initOutput] { // copy initOutput here to avoid memory corruption
        scriptThreadFunc(initOutput);
    });
}

void ScriptContainerJS::run(const ScriptInputArgs& in, const ScriptOutputArgs& out) {
    std::unique_lock<std::mutex> lock{mTasksMutex};
    mTasks.emplace(in, out);
    lock.unlock();
    mTasksCV.notify_all();
}

void ScriptContainerJS::scriptThreadFunc(const ScriptInitOutputArgs& initOutput) {
    JS_UpdateStackTop(mJSRuntime);
    auto ret = JS_Eval(mJSContext, mCode.c_str(), mCode.size(), mName.c_str(), 0);

    util::DestructWrapper destructRet{[&]{ JS_FreeValue(mJSContext, ret); }};
    if(JS_IsException(ret)) {
        initOutput.error(std::string{"Script error: "} + getJSException());
        return;
    }
    auto runType = getJSStringProperty(ret, "runType");
    if(!runType) {
        initOutput.error("Init return runType is not a string");
        return;
    }
    if(runType == "sync") {
        mScriptInitReturn.runType = ScriptRunType::Sync;
    } else if(runType == "async") {
        mScriptInitReturn.runType = ScriptRunType::Async;
    } else {
        initOutput.error(std::string{"Init return runType should be 'sync' or 'async', not '"} + *runType + "'");
        return;
    }

    auto actionsObj = JS_GetPropertyStr(mJSContext, ret, "actions");
    util::DestructWrapper destructActionsObj{[&]{ JS_FreeValue(mJSContext, actionsObj); }};
    if(JS_IsArray(mJSContext, actionsObj)) {
        handleScriptActions(actionsObj, initOutput.initialActionsOutput);
    } else if(!JS_IsUndefined(actionsObj)) {
        initOutput.error("Initial actions must be an array!");
        return;
    }

    destructRet.execute();

    initOutput.success(mScriptInitReturn);

    // start run loop
    while(!mShouldAbort) {
        std::unique_lock<std::mutex> lock{mTasksMutex};
        if(mTasks.empty()) {
           mTasksCV.wait(lock);
        }
        if(mShouldAbort) {
            break;
        }
        auto[input, output] = std::move(mTasks.front());
        mTasks.pop();
        lock.unlock();
        performRun(output);
    }
}
void ScriptContainerJS::forceQuit() {
    mShouldAbort = true;
    mTasksCV.notify_all();
    if(mScriptThread)
        mScriptThread->join();
    mScriptThread.reset();
}
void ScriptContainerJS::performRun(const ScriptOutputArgs& output) {

    auto globalObj = JS_GetGlobalObject(mJSContext);
    util::DestructWrapper destructGlobalObj{[&]{ JS_FreeValue(mJSContext, globalObj); }};

    auto runFunction = JS_GetPropertyStr(mJSContext, globalObj, "run");
    util::DestructWrapper destructRunFunction{[&]{ JS_FreeValue(mJSContext, runFunction); }};
    if(!JS_IsFunction(mJSContext, runFunction)) {
        output.error("no run function defined!");
        return;
    }
    auto runResult = JS_Call(mJSContext, runFunction, globalObj, 0, nullptr);
    util::DestructWrapper destructRunResult{[&]{ JS_FreeValue(mJSContext, runResult); }};

    destructGlobalObj.execute();

    handleScriptActions(runResult, output);
}

std::string ScriptContainerJS::getJSException() {
    auto exception = JS_GetException(mJSContext);
    auto exceptionString = JS_ToCString(mJSContext, exception);
    std::string ret = exceptionString;
    JS_FreeCString(mJSContext, exceptionString);
    JS_FreeValue(mJSContext, exception);
    return ret;
}

void ScriptContainerJS::handleScriptActions(const JSValue& actions, const ScriptOutputArgs& output) {
    if(!JS_IsArray(mJSContext, actions)) {
        if(JS_IsException(actions)) {
            output.error(getJSException());
            return;
        }
        output.error("run function didn't return an array!");
        return;
    }

    // perform each action
    uint32_t i = 0;
    while(true) {
        auto action = JS_GetPropertyUint32(mJSContext, actions, i);
        util::DestructWrapper destructAction{[&]{ JS_FreeValue(mJSContext, action); }};
        if(JS_IsUndefined(action)) {
            break;
        }
        if(!JS_IsObject(action)) {
            output.error("One returned action wasn't an object!");
            break;
        }
        auto maybeActionTypeStr = getJSStringProperty(action, "type");
        if(!maybeActionTypeStr) {
            output.error("type property is not a string");
            break;
        }
        auto actionType = *maybeActionTypeStr;
        if(actionType == "publish") {
            auto topic = getJSStringProperty(action, "topic");
            if(!topic) {
                output.error("topic property is not a string");
                break;
            }
            auto qosVal = JS_GetPropertyStr(mJSContext, action, "qos");
            util::DestructWrapper destructQosVal{[&]{ JS_FreeValue(mJSContext, qosVal); }};
            if(!JS_IsNumber(qosVal)) {
                output.error("qos must be a number");
                break;
            }
            int32_t qosNum;
            if(JS_ToInt32(mJSContext, &qosNum, qosVal) < 0) {
                output.error("qos must be a number");
                break;
            }
            if(qosNum < 0 || qosNum >= 3) {
                output.error("qos must be between 0 and 2");
                break;
            }
            QoS qos = static_cast<QoS>(qosNum);

            auto retainObj = JS_GetPropertyStr(mJSContext, action, "retain");
            util::DestructWrapper destructRetainObj{[&]{ JS_FreeValue(mJSContext, retainObj); }};
            if(!JS_IsBool(retainObj)) {
                output.error("retain must be a bool");
                break;
            }

            int retainInt = 0;
            if((retainInt = JS_ToBool(mJSContext, retainObj)) < 0) {
                output.error("retain must be a bool");
                break;
            }

            Retain retain = retainInt == 0 ? Retain::No : Retain::Yes;

            std::vector<uint8_t> payload;
            auto payloadStr = getJSStringProperty(action, "payloadStr");
            if(payloadStr) {
                payload = std::vector<uint8_t>{payloadStr->cbegin(), payloadStr->cend()};
            } else {
                // we need either payloadStr or payloadBytes, so check now
                auto payloadBytesObj = JS_GetPropertyStr(mJSContext, action, "payloadBytes");
                util::DestructWrapper destructPayloadBytesObj{[&]{ JS_FreeValue(mJSContext, payloadBytesObj); }};
                size_t bytesSize = 0;
                size_t bytesPerElement = 0;
                size_t bytesOffset = 0;
                auto payloadBytesValue = JS_GetTypedArrayBuffer(mJSContext, payloadBytesObj, &bytesOffset, &bytesSize, &bytesPerElement);
                util::DestructWrapper destructPayloadBytesValue{[&]{ JS_FreeValue(mJSContext, payloadBytesValue); }};
                auto payloadBytes = JS_GetArrayBuffer(mJSContext, &bytesSize, payloadBytesValue);
                if(bytesPerElement != 1 || payloadBytes == nullptr) {
                    output.error("missing either a payloadStr (string) or payloadBytes (Uint8Array)");
                    break;
                }
                payload = std::vector<uint8_t>{payloadBytes, payloadBytes + bytesSize};
            }
            output.publish(std::move(*topic), std::move(payload), qos, retain);

        } else if(actionType == "subscribe") {
            auto topic = getJSStringProperty(action, "topic");
            if(!topic) {
                output.error("topic field is missing");
                break;
            }
            output.subscribe(*topic);

        } else if(actionType == "unsubscribe") {
            auto topic = getJSStringProperty(action, "topic");
            if(!topic) {
                output.error("topic field is missing");
                break;
            }
            output.unsubscribe(*topic);

        }
        i += 1;
    }
    output.success();
}

std::optional<std::string> ScriptContainerJS::getJSStringProperty(const JSValue& obj, std::string_view name) {
    auto prop = JS_GetPropertyStr(mJSContext, obj, name.data());
    util::DestructWrapper destructProp{[&]{ JS_FreeValue(mJSContext, prop); }};
    if(!JS_IsString(prop)) {
        return {};
    }
    auto propStr = JS_ToCString(mJSContext, prop);
    util::DestructWrapper destructPropStr{[&]{ JS_FreeCString(mJSContext, propStr); }};
    std::string ret = propStr;
    return {std::move(ret)};
}

}