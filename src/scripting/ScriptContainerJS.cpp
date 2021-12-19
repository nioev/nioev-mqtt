#include "ScriptContainerJS.hpp"
#include "quickjs.h"
#include "../Util.hpp"
#include "spdlog/spdlog.h"
#include "../ApplicationState.hpp"

namespace nioev {

ScriptContainerJS::ScriptContainerJS(ApplicationState& p, const std::string& scriptName, std::string&& scriptCode)
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
            try {
                ScriptContainerJS* self = reinterpret_cast<ScriptContainerJS*>(JS_GetContextOpaque(ctx));
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
                return JS_NewString(ctx, e.what());
            }
    }, "log", 0);

    auto logErrFunc = JS_NewCFunction(mJSContext, [](JSContext* ctx, JSValue this_obj, int argc, JSValue* args) {
            try {
                ScriptContainerJS* self = reinterpret_cast<ScriptContainerJS*>(JS_GetContextOpaque(ctx));
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
                return JS_NewString(ctx, e.what());
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
        // publish
        auto& cppParams = std::get<0>(input);
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
    case 1: {
        // new tcp
        auto& cppParams = std::get<1>(input);
        JS_SetPropertyStr(mJSContext, paramObj, "type", JS_NewString(mJSContext, "tcp_new_client"));
        JS_SetPropertyStr(mJSContext, paramObj, "fd", JS_NewInt32(mJSContext, cppParams.fd));
        break;
    }
    case 2: {
        // new tcp msg
        auto& cppParams = std::get<2>(input);
        JS_SetPropertyStr(mJSContext, paramObj, "type", JS_NewString(mJSContext, "tcp_new_msg"));
        // TODO
        break;
    }
    case 3: {
        // delete tcp
        auto& cppParams = std::get<3>(input);
        JS_SetPropertyStr(mJSContext, paramObj, "type", JS_NewString(mJSContext, "tcp_delete_client"));
        JS_SetPropertyStr(mJSContext, paramObj, "fd", JS_NewInt32(mJSContext, cppParams.fd));
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
            mApp.requestChange(ChangeRequestPublish{std::move(*topic), std::move(*payload), qos, retain});

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

        } else if(actionType == "tcp_listen") {
            auto identifier = getJSStringProperty(action, "identifier");
            if(!identifier) {
                status.error(mName, "identifier field is missing");
                return;
            }
            auto sendCompressionStr = getJSStringProperty(action, "send_compression");
            Compression sendCompression = Compression::NONE;
            if(sendCompressionStr) {
              if(sendCompressionStr == "zstd") {
                  sendCompression = Compression::ZSTD;
              } else if(sendCompressionStr == "none") {
                  sendCompression = Compression::NONE;
              } else {
                  status.error(mName, "send_compression must be zstd or none");
              }
            }
            auto recvCompressionStr = getJSStringProperty(action, "recv_compression");
            Compression recvCompression = Compression::NONE;
            if(recvCompressionStr) {
                if(recvCompressionStr == "none") {
                    recvCompression = Compression::NONE;
                } else {
                    status.error(mName, "recv_compression must be none");
                }
            }
            spdlog::warn("tcp_listen currently not implemented");

        } else if(actionType == "tcp_send") {
            auto fd = getJSIntProperty(action, "fd");
            if(!fd) {
                status.error(mName, "fd field is missing");
                return;
            }
            auto payload = extractPayload(action);
            if(!payload) {
                status.error(mName, "Missing either payloadStr or payloadBytes");
                return;
            }
            spdlog::warn("tcp_send currently not implemented");

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