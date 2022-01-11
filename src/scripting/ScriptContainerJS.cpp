#include "ScriptContainerJS.hpp"
#include "quickjs.h"
#include "../Util.hpp"
#include "spdlog/spdlog.h"
#include "../ApplicationState.hpp"

namespace nioev {

ScriptContainerJS::ScriptContainerJS(ApplicationState& p, const std::string& scriptName, std::string&& scriptCode)
: ScriptContainer(p, std::move(scriptCode)), mName(scriptName) {
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
    while(!mTimeouts.empty()) {
        mTimeouts.top().freeJSValues(mJSContext);
        mTimeouts.pop();
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
    if(!mInitFailureMessage.empty()) {
        status.error(mName, "Init failed: " + mInitFailureMessage);
        return;
    }
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

    initStatus.error = [this, error = std::move(initStatus.error)](const std::string& scriptName, const std::string& errorMsg) {
        std::unique_lock<std::mutex> lock{mTasksMutex};
        mInitFailureMessage = errorMsg;
        lock.unlock();
        error(mName, errorMsg);
    };

    auto logFunc = JS_NewCFunction(mJSContext, [](JSContext* ctx, JSValue this_obj, int argc, JSValue* args) {
            ScriptContainerJS* self = reinterpret_cast<ScriptContainerJS*>(JS_GetContextOpaque(ctx));
            try {
                std::string toLog;
                for(int i = 0; i < argc; ++i) {
                    auto json = JS_JSONStringify(ctx, args[i], JS_UNDEFINED, JS_UNDEFINED);
                    util::DestructWrapper destructJson{[&]{ JS_FreeValue(ctx, json); }};
                    auto cStr = JS_ToCString(ctx, json);
                    util::DestructWrapper destructCStr{[&]{ JS_FreeCString(ctx, cStr); }};
                    toLog += cStr;
                    if(i != argc - 1)
                        toLog += " ";
                }
                spdlog::info("[SCRIPT] [{}] {}", self->mName, toLog);
                return JS_UNDEFINED;
            } catch (std::exception& e) {
                return JS_Throw(ctx, JS_NewString(ctx, e.what()));
            }
    }, "log", 0);

    auto logErrFunc = JS_NewCFunction(mJSContext, [](JSContext* ctx, JSValue this_obj, int argc, JSValue* args) {
            ScriptContainerJS* self = reinterpret_cast<ScriptContainerJS*>(JS_GetContextOpaque(ctx));
            try {
                std::string toLog;
                for(int i = 0; i < argc; ++i) {
                    auto json = JS_JSONStringify(ctx, args[i], JS_UNDEFINED, JS_UNDEFINED);
                    util::DestructWrapper destructJson{[&]{ JS_FreeValue(ctx, json); }};
                    auto cStr = JS_ToCString(ctx, json);
                    util::DestructWrapper destructCStr{[&]{ JS_FreeCString(ctx, cStr); }};
                    toLog += cStr;
                    if(i != argc - 1)
                        toLog += " ";
                }
                spdlog::error("[SCRIPT] [{}] {}", self->mName, toLog);
                return JS_UNDEFINED;
            } catch (std::exception& e) {
                return JS_Throw(ctx, JS_NewString(ctx, e.what()));
            }
        }, "error", 0);
    //util::DestructWrapper destructLogFunc{[&]{ JS_FreeValue(mJSContext, logFunc); }};
    JS_SetContextOpaque(mJSContext, this);

    auto globalObj = JS_GetGlobalObject(mJSContext);
    util::DestructWrapper destructGlobalObj{[&]{ JS_FreeValue(mJSContext, globalObj); }};
    auto console = JS_NewObject(mJSContext);
    JS_SetPropertyStr(mJSContext, globalObj, "console", console);
    JS_SetPropertyStr(mJSContext, console, "log", logFunc);
    JS_SetPropertyStr(mJSContext, console, "error", logErrFunc);

    auto loadNativeLibFunc = JS_NewCFunction(mJSContext, [](JSContext* ctx, JSValue this_obj, int argc, JSValue* args) {
            ScriptContainerJS* self = reinterpret_cast<ScriptContainerJS*>(JS_GetContextOpaque(ctx));
            try {
                if(argc < 1) {
                    throw std::runtime_error{"You need to pass the library that you want to load"};
                }
                if(!JS_IsString(args[0])) {
                    throw std::runtime_error{"First argument needs to be a string"};
                }
                auto libName = JS_ToCString(ctx, args[0]);
                util::DestructWrapper destructLibName{[&] {
                    JS_FreeCString(ctx, libName);
                }};
                std::string libNameStr = libName;
                while(true) {
                    auto[loadingLibs, lock] = self->mApp.getListOfCurrentlyLoadingNativeLibs();
                    if(loadingLibs.get().contains(libNameStr)) {
                        lock.unlock();
                        spdlog::warn("[{}] Stalling execution while waiting for native library {} to compile", self->mName, libNameStr);
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    } else {
                        break;
                    }
                }
                auto existingLib = self->mNativeLibs.find(libNameStr);
                if(existingLib != self->mNativeLibs.end()) {
                    self->mNativeLibs.erase(existingLib);
                }
                auto lib = self->mNativeLibs.emplace_hint(existingLib, libNameStr, "libs/lib" + libNameStr + ".so");
                auto features = lib->second.getFeatures();
                if(std::find(features.cbegin(), features.cend(), "js") == features.cend()) {
                    throw std::runtime_error{"Library doesn't support js"};
                }
                auto nativeFunction = (JSCFunction*) lib->second.getFP("_nioev_library_function_load_js");
                // if while debugging you see a crash in the line below, that probably means that your native code is at fault!
                return nativeFunction(ctx, this_obj, argc - 1, args + 1);
            } catch (std::exception& e) {
                return JS_Throw(ctx, JS_NewString(ctx, e.what()));
            }
        }, "loadNativeLibrary", 1);
    auto nv = JS_NewObject(mJSContext);
    JS_SetPropertyStr(mJSContext, globalObj, "nv", nv);
    JS_SetPropertyStr(mJSContext, nv, "loadNativeLibrary", loadNativeLibFunc);

    auto setTimeoutFunc = JS_NewCFunction(mJSContext, [](JSContext* ctx, JSValue this_obj, int argc, JSValue* args) {
            ScriptContainerJS* self = reinterpret_cast<ScriptContainerJS*>(JS_GetContextOpaque(ctx));
            try {
                if(argc < 1) {
                    throw std::runtime_error{"You need to pass the function to call"};
                }
                JSValue function = args[0];
                if(!JS_IsFunction(ctx, function)) {
                    throw std::runtime_error{"Argument 0 to setTimeout must be a function"};
                }
                int32_t timeout = 0;
                if(argc >= 2) {
                    if(JS_ToInt32(ctx, &timeout, args[1]) < 0) {
                        throw std::runtime_error{"Timeout must be an int32!"};
                    }
                }
                auto id = self->mTimeoutIdCounter++;
                if(self->mTimeoutIdCounter < 0) {
                    self->mTimeoutIdCounter = 1; // wrap around
                }
                std::vector<JSValue> passedArgs;
                passedArgs.reserve(argc - 2);
                for(int i = 2; i < argc; ++i) {
                    passedArgs.push_back(JS_DupValue(ctx, args[i]));
                }
                self->mTimeouts.emplace(TimeoutData{std::chrono::milliseconds(timeout), id, JS_DupValue(ctx, function), std::move(passedArgs)});
                return JS_NewInt32(ctx, id);
            } catch (std::exception& e) {
                return JS_Throw(ctx, JS_NewString(ctx, e.what()));
            }
        }, "setTimeout", 1);
    JS_SetPropertyStr(mJSContext, globalObj, "setTimeout", setTimeoutFunc);

    auto clearTimeoutFunc = JS_NewCFunction(mJSContext, [](JSContext* ctx, JSValue this_obj, int argc, JSValue* args) {
            ScriptContainerJS* self = reinterpret_cast<ScriptContainerJS*>(JS_GetContextOpaque(ctx));
            try {
                if(argc < 1) {
                    throw std::runtime_error{"You need to pass an id"};
                }
                int32_t id;
                if(JS_ToInt32(ctx, &id, args[0]) < 0) {
                    throw std::runtime_error{"Id must be an int32!"};
                }
                auto timeoutsCpy = std::move(self->mTimeouts);
                self->mTimeouts = {};
                while(!timeoutsCpy.empty()) {
                    if(timeoutsCpy.top().mId != id) {
                        self->mTimeouts.push(timeoutsCpy.top());
                    } else {
                        timeoutsCpy.top().freeJSValues(ctx);
                    }
                    timeoutsCpy.pop();
                }
                return JS_UNDEFINED;
            } catch (std::exception& e) {
                return JS_Throw(ctx, JS_NewString(ctx, e.what()));
            }
        }, "clearTimeout", 1);
    JS_SetPropertyStr(mJSContext, globalObj, "clearTimeout", clearTimeoutFunc);
    JS_SetPropertyStr(mJSContext, globalObj, "setInterval", JS_UNDEFINED);
    JS_SetPropertyStr(mJSContext, globalObj, "clearInterval", JS_UNDEFINED);

    auto ret = JS_Eval(mJSContext, mCode.c_str(), mCode.size(), mName.c_str(), 0);
    util::DestructWrapper destructRet{[&]{ JS_FreeValue(mJSContext, ret); }};

    if(JS_IsException(ret)) {
        initStatus.error(mName, std::string{"Script error: "} + getJSException());
        return;
    }
    auto runType = getJSStringProperty(ret, "runType");
    if(!runType) {
        runType = "async";
    }
    {
        std::lock_guard<std::mutex> lock{mScriptInitReturnMutex};
        mScriptInitReturn.emplace();
        if(runType == "sync") {
            mScriptInitReturn->runType = ScriptRunType::Sync;
        } else if(runType == "async") {
            mScriptInitReturn->runType = ScriptRunType::Async;
        } else {
            initStatus.error(mName, std::string{"Init return runType should be 'sync' or 'async', not '"} + *runType + "'");
            return;
        }
    }

    auto actionsObj = JS_GetPropertyStr(mJSContext, ret, "actions");
    util::DestructWrapper destructActionsObj{[&]{ JS_FreeValue(mJSContext, actionsObj); }};
    handleScriptActions(actionsObj, std::move(initStatus));

    destructActionsObj.execute();
    destructRet.execute();

    // start run loop
    while(!mShouldAbort) {
        std::unique_lock<std::mutex> lock{mTasksMutex};
        while(mTasks.empty()) {
            if(!mTimeouts.empty()) {
                auto waitFor = mTimeouts.top().timeLeftUntilShouldBeRun();
                if(waitFor > std::chrono::milliseconds(0)) {
                    mTasksCV.wait_for(lock, waitFor);
                } else {
                    auto timeout = mTimeouts.top();
                    mTimeouts.pop();
                    auto timeoutCallRet = JS_Call(mJSContext, timeout.mFunction, globalObj, timeout.mFunctionArgs.size(), timeout.mFunctionArgs.data());
                    JS_FreeValue(mJSContext, timeoutCallRet);
                    timeout.freeJSValues(mJSContext);
                }
            } else {
                mTasksCV.wait(lock);
            }
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

    JSValue runFunction = JS_UNDEFINED;
    util::DestructWrapper destructRunFunction{[&]{ JS_FreeValue(mJSContext, runFunction); }};
    auto loadRunFunction = [&](const char* name) {
        auto func = JS_GetPropertyStr(mJSContext, globalObj, name);
        if(!JS_IsFunction(mJSContext, runFunction)) {
            status.error(mName, "function " + std::string{name} + " not defined!");
            return;
        }
    };
    auto paramObj = JS_NewObject(mJSContext);
    util::DestructWrapper destructParamObj{[&]{ JS_FreeValue(mJSContext, paramObj); }};

    switch(input.index()) {
    case 0: {
        // publish
        loadRunFunction("onPublish");
        auto& cppParams = std::get<0>(input);
        JS_SetPropertyStr(mJSContext, paramObj, "type", JS_NewString(mJSContext, "publish"));
        JS_SetPropertyStr(mJSContext, paramObj, "topic", JS_NewString(mJSContext, cppParams.topic.c_str()));
        JS_SetPropertyStr(mJSContext, paramObj, "retained", JS_NewBool(mJSContext, cppParams.retained == Retained::Yes));

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

    if(mScriptInitReturn->runType == ScriptRunType::Sync) {
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
            auto qosNum = getJSIntProperty(action, "qos");
            if(!qosNum) {
                qosNum = 0;
            }
            if(*qosNum < 0 || *qosNum >= 3) {
                status.error(mName, "qos must be between 0 and 2");
                return;
            }
            QoS qos = static_cast<QoS>(*qosNum);

            auto retainBool = getJSBoolProperty(action, "retain");
            if(!retainBool.has_value()) {
                retainBool = false;
            }
            Retain retain = *retainBool ? Retain::Yes : Retain::No;

            auto payload = extractPayload(action);
            if(!payload) {
                status.error(mName, "Missing either payloadStr or payloadBytes");
                return;
            }
            mApp.publishAsync(AsyncPublishData{std::move(*topic), std::move(*payload), qos, retain});

        } else if(actionType == "subscribe") {
            auto topic = getJSStringProperty(action, "topic");
            if(!topic) {
                status.error(mName, "topic field is missing");
                return;
            }
            mApp.requestChange(ChangeRequestSubscribe{makeShared(), *topic});

        } else if(actionType == "unsubscribe") {
            auto topic = getJSStringProperty(action, "topic");
            if(!topic) {
                status.error(mName, "topic field is missing");
                return;
            }
            mApp.requestChange(ChangeRequestUnsubscribe{makeShared(), *topic});

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

std::optional<bool> ScriptContainerJS::getJSBoolProperty(const JSValue& obj, std::string_view name) {
    auto prop = JS_GetPropertyStr(mJSContext, obj, name.data());
    util::DestructWrapper destructProp{[&]{ JS_FreeValue(mJSContext, prop); }};
    if(!JS_IsBool(prop))
        return {};
    auto ret = JS_ToBool(mJSContext, prop);
    if(ret < 0) {
        return {};
    }
    return ret;
}

std::optional<int32_t> ScriptContainerJS::getJSIntProperty(const JSValue& obj, std::string_view name) {
    auto prop = JS_GetPropertyStr(mJSContext, obj, name.data());
    util::DestructWrapper destructProp{[&]{ JS_FreeValue(mJSContext, prop); }};
    if(!JS_IsNumber(prop))
        return {};
    int32_t ret = 0;
    if(JS_ToInt32(mJSContext, &ret, prop) < 0)
        return {};
    return ret;
}
std::optional<std::vector<uint8_t>> ScriptContainerJS::extractPayload(const JSValue& action) {
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
            return {};
        }
        auto payloadBytes = JS_GetArrayBuffer(mJSContext, &bytesSize, payloadBytesValue);
        if(bytesPerElement != 1 || (payloadBytes == nullptr && bytesSize != 0)) {
            return {};
        }
        payload = std::vector<uint8_t>{payloadBytes, payloadBytes + bytesSize};
    }
    return payload;
}

}