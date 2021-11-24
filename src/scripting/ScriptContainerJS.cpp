#include "ScriptContainerJS.hpp"
#include "quickjs.h"
#include "../Util.hpp"
#include "spdlog/spdlog.h"
#include "ScriptActionPerformer.hpp"

namespace nioev {

ScriptContainerJS::ScriptContainerJS(ScriptActionPerformer& p, const std::string& scriptName, std::string&& scriptCode)
: ScriptContainer(p), mName(scriptName), mCode(std::move(scriptCode)) {
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
    if(mScriptThread) {
        std::unique_lock<std::mutex> lock{mTasksMutex};
        mShouldAbort = true;
        mTasksCV.notify_all();
        lock.unlock();
        mScriptThread->join();
    }
    JS_FreeContext(mJSContext);
    mJSContext = nullptr;
    JS_FreeRuntime(mJSRuntime);
    mJSRuntime = nullptr;
}

void ScriptContainerJS::init(ScriptStatusOutput&& status) {
    mScriptThread.emplace([this, status = std::move(status)] () mutable { // copy initOutput here to avoid memory corruption
        scriptThreadFunc(std::move(status));
    });
}

void ScriptContainerJS::run(const ScriptInputArgs& in, ScriptStatusOutput&& status) {
    std::unique_lock<std::mutex> lock{mTasksMutex};
    mTasks.emplace(in, std::move(status));
    lock.unlock();
    mTasksCV.notify_all();
}

void ScriptContainerJS::scriptThreadFunc(ScriptStatusOutput&& initStatus) {
    {
        std::string threadName = "S-" + mName;
        pthread_setname_np(pthread_self(), threadName.c_str());
    }
    JS_UpdateStackTop(mJSRuntime);
    auto ret = JS_Eval(mJSContext, mCode.c_str(), mCode.size(), mName.c_str(), 0);
    util::DestructWrapper destructRet{[&]{ JS_FreeValue(mJSContext, ret); }};

    if(JS_IsException(ret)) {
        initStatus.error(mName, std::string{"Script error: "} + getJSException());
        return;
    }
    auto runType = getJSStringProperty(ret, "runType");
    if(!runType) {
        initStatus.error(mName, "Init return runType is not a string");
        return;
    }
    if(runType == "sync") {
        mScriptInitReturn.runType = ScriptRunType::Sync;
    } else if(runType == "async") {
        mScriptInitReturn.runType = ScriptRunType::Async;
    } else {
        initStatus.error(mName, std::string{"Init return runType should be 'sync' or 'async', not '"} + *runType + "'");
        return;
    }

    auto actionsObj = JS_GetPropertyStr(mJSContext, ret, "actions");
    util::DestructWrapper destructActionsObj{[&]{ JS_FreeValue(mJSContext, actionsObj); }};
    if(JS_IsArray(mJSContext, actionsObj)) {
        handleScriptActions(actionsObj, std::move(initStatus));
    } else if(!JS_IsUndefined(actionsObj)) {
        initStatus.error(mName, "Initial actions must be an array!");
        return;
    }

    destructActionsObj.execute();
    destructRet.execute();

    // start run loop
    while(!mShouldAbort) {
        std::unique_lock<std::mutex> lock{mTasksMutex};
        while(mTasks.empty()) {
           mTasksCV.wait(lock);
           if(mShouldAbort) {
               return;
           }
        }
        auto[input, status] = std::move(mTasks.front());
        mTasks.pop();
        lock.unlock();
        performRun(input, std::move(status));
    }
}
void ScriptContainerJS::forceQuit() {
    mShouldAbort = true;
    mTasksCV.notify_all();
    if(mScriptThread)
        mScriptThread->join();
    mScriptThread.reset();
}
void ScriptContainerJS::performRun(const ScriptInputArgs& input, ScriptStatusOutput&& status) {

    auto globalObj = JS_GetGlobalObject(mJSContext);
    util::DestructWrapper destructGlobalObj{[&]{ JS_FreeValue(mJSContext, globalObj); }};

    auto runFunction = JS_GetPropertyStr(mJSContext, globalObj, "run");
    util::DestructWrapper destructRunFunction{[&]{ JS_FreeValue(mJSContext, runFunction); }};
    if(!JS_IsFunction(mJSContext, runFunction)) {
        status.error(mName, "no run function defined!");
        return;
    }
    auto paramObj = JS_NewObject(mJSContext);
    util::DestructWrapper destructParamObj{[&]{ JS_FreeValue(mJSContext, paramObj); }};

    switch(input.index()) {
    case 0: {
        auto& cppParams = std::get<ScriptRunArgsMqttMessage>(input);
        JS_SetPropertyStr(mJSContext, paramObj, "type", JS_NewString(mJSContext, "publish"));
        JS_SetPropertyStr(mJSContext, paramObj, "topic", JS_NewString(mJSContext, cppParams.topic.c_str()));

        auto arrayBuffer = JS_NewArrayBuffer(mJSContext, const_cast<uint8_t*>(cppParams.payload.data()), cppParams.payload.size(), [](JSRuntime*, void*, void*) {}, nullptr, true);
        util::DestructWrapper destructArrayBuffer{[&]{ JS_FreeValue(mJSContext, arrayBuffer); }};

        auto uint8ArrayFunc = JS_GetPropertyStr(mJSContext, globalObj, "Uint8Array");
        util::DestructWrapper destructUint8ArrayFunc{[&]{ JS_FreeValue(mJSContext, uint8ArrayFunc); }};

        JS_SetPropertyStr(mJSContext, paramObj, "payloadBytes", JS_CallConstructor(mJSContext, uint8ArrayFunc, 1, &arrayBuffer));
        if(cppParams.payload.empty()) {
            JS_SetPropertyStr(mJSContext, paramObj, "payloadStr", JS_NewString(mJSContext, ""));
        } else {
            JS_SetPropertyStr(mJSContext, paramObj, "payloadStr", JS_NewStringLen(mJSContext, (char*)cppParams.payload.data(), cppParams.payload.size()));
        }
        break;
    }
    default:
        assert(false);
    }
    auto runResult = JS_Call(mJSContext, runFunction, globalObj, 1, &paramObj);
    util::DestructWrapper destructRunResult{[&]{ JS_FreeValue(mJSContext, runResult); }};

    auto actions = JS_GetPropertyStr(mJSContext, runResult, "actions");
    util::DestructWrapper destructActions{[&]{ JS_FreeValue(mJSContext, actions); }};

    destructGlobalObj.execute();

    if(mScriptInitReturn.runType == ScriptRunType::Sync) {
        auto syncActionStr = getJSStringProperty(runResult, "syncAction");
        if(syncActionStr) {
            if(syncActionStr == "continue") {
                status.syncAction(mName, SyncAction::Continue);
            } else if(syncActionStr == "abortPublish") {
                status.syncAction(mName, SyncAction::AbortPublish);
            } else {
                status.error(mName, "syncAction must be 'continue' or 'abortPublish'");
                return;
            }
        } else {
            status.syncAction(mName, SyncAction::Continue);
        }
    }
    handleScriptActions(actions, std::move(status));
}

std::string ScriptContainerJS::getJSException() {
    auto exception = JS_GetException(mJSContext);
    auto exceptionString = JS_ToCString(mJSContext, exception);
    std::string ret = exceptionString;
    JS_FreeCString(mJSContext, exceptionString);
    JS_FreeValue(mJSContext, exception);
    return ret;
}

void ScriptContainerJS::handleScriptActions(const JSValue& actions, ScriptStatusOutput&& status) {
    if(JS_IsUndefined(actions)) {
        status.success(mName);
        return;
    }
    if(!JS_IsArray(mJSContext, actions)) {
        if(JS_IsException(actions)) {
            status.error(mName, getJSException());
            return;
        }
        status.error(mName, "Function didn't return an array!");
        return;
    }

    // perform each action
    uint32_t i = 0;
    while(true) {
        auto action = JS_GetPropertyUint32(mJSContext, actions, i);
        util::DestructWrapper destructAction{[&]{ JS_FreeValue(mJSContext, action); }};
        if(JS_IsUndefined(action)) {
            status.success(mName);
            break;
        }
        if(!JS_IsObject(action)) {
            status.error(mName, "One returned action wasn't an object!");
            return;
        }
        auto maybeActionTypeStr = getJSStringProperty(action, "type");
        if(!maybeActionTypeStr) {
            status.error(mName, "type property is not a string");
            return;
        }
        auto actionType = *maybeActionTypeStr;
        if(actionType == "publish") {
            auto topic = getJSStringProperty(action, "topic");
            if(!topic) {
                status.error(mName, "topic property is not a string");
                return;
            }
            auto qosVal = JS_GetPropertyStr(mJSContext, action, "qos");
            util::DestructWrapper destructQosVal{[&]{ JS_FreeValue(mJSContext, qosVal); }};
            if(!JS_IsNumber(qosVal)) {
                status.error(mName, "qos must be a number");
                return;
            }
            int32_t qosNum;
            if(JS_ToInt32(mJSContext, &qosNum, qosVal) < 0) {
                status.error(mName, "qos must be a number");
                return;
            }
            if(qosNum < 0 || qosNum >= 3) {
                status.error(mName, "qos must be between 0 and 2");
                return;
            }
            QoS qos = static_cast<QoS>(qosNum);

            auto retainObj = JS_GetPropertyStr(mJSContext, action, "retain");
            util::DestructWrapper destructRetainObj{[&]{ JS_FreeValue(mJSContext, retainObj); }};
            if(!JS_IsBool(retainObj)) {
                status.error(mName, "retain must be a bool");
                return;
            }

            int retainInt = 0;
            if((retainInt = JS_ToBool(mJSContext, retainObj)) < 0) {
                status.error(mName, "retain must be a bool");
                return;
            }

            Retain retain = retainInt == 0 ? Retain::No : Retain::Yes;

            std::vector<uint8_t> payload;
            auto payloadStr = getJSStringProperty(action, "payloadStr");
            if(payloadStr) {
                payload = std::vector<uint8_t>{payloadStr->cbegin(), payloadStr->cend()};
            } else {
                // we need either payloadStr or payloadBytes, so check now
                auto payloadBytesObj = JS_GetPropertyStr(mJSContext, action, "payloadBytes");
                //spdlog::info("Payload: '{}'", JS_ToCString(mJSContext, JS_JSONStringify(mJSContext, payloadBytesObj, JS_NewString(mJSContext, ""), JS_NewString(mJSContext, ""))));
                util::DestructWrapper destructPayloadBytesObj{[&]{ JS_FreeValue(mJSContext, payloadBytesObj); }};
                size_t bytesSize = 0;
                size_t bytesPerElement = 0;
                size_t bytesOffset = 0;
                auto payloadBytesValue = JS_GetTypedArrayBuffer(mJSContext, payloadBytesObj, &bytesOffset, &bytesSize, &bytesPerElement);
                util::DestructWrapper destructPayloadBytesValue{[&]{ JS_FreeValue(mJSContext, payloadBytesValue); }};
                if(JS_IsException(payloadBytesValue)) {
                    status.error(mName, "missing either a payloadStr (string) or payloadBytes (Uint8Array)");
                    return;
                }
                auto payloadBytes = JS_GetArrayBuffer(mJSContext, &bytesSize, payloadBytesValue);
                if(bytesPerElement != 1 || (payloadBytes == nullptr && bytesSize != 0)) {
                    status.error(mName, "missing either a payloadStr (string) or payloadBytes (Uint8Array)");
                    return;
                }
                payload = std::vector<uint8_t>{payloadBytes, payloadBytes + bytesSize};
            }
            mActionPerformer.enqueueAction(ScriptActionPublish{mName, std::move(*topic), std::move(payload), qos, retain});

        } else if(actionType == "subscribe") {
            auto topic = getJSStringProperty(action, "topic");
            if(!topic) {
                status.error(mName, "topic field is missing");
                return;
            }
            mActionPerformer.enqueueAction(ScriptActionSubscribe{mName, *topic});

        } else if(actionType == "unsubscribe") {
            auto topic = getJSStringProperty(action, "topic");
            if(!topic) {
                status.error(mName, "topic field is missing");
                return;
            }
            mActionPerformer.enqueueAction(ScriptActionUnsubscribe{mName, *topic});

        }
        i += 1;
    }
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