#include "REST.hpp"
#include "App.h"
#include "ApplicationState.hpp"
#include "HttpResponse.h"
#include "quickjs_h_embedded.hpp"
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace nioev {

struct PerWebsocketClientData {
    std::string topic;
};

class WSSubscriber : public nioev::Subscriber {
public:
    WSSubscriber(ApplicationState& app, uWS::Loop& loop, uWS::WebSocket<false, true, PerWebsocketClientData>* ws) : mApp(app), mLoop(loop), mWS(ws) { }
    void publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained, MQTTPublishPacketBuilder&) override {
        mLoop.defer([this, topic, payload, active = mActive] {
            if(!*active)
                return;
            mWS->send(std::string_view{ (const char*)payload.data(), payload.size() }, uWS::BINARY, false);
            // mWebApp.publish(mWSId, std::string_view{ (const char*)payload.data(), payload.size() }, uWS::BINARY, false);
        });
    }
    const char* getType() const override {
        return "ws";
    }
    void deactivate() {
        *mActive = false;
    }

private:
    ApplicationState& mApp;
    uWS::Loop& mLoop;
    uWS::WebSocket<false, true, PerWebsocketClientData>* mWS;
    std::shared_ptr<bool> mActive{ std::make_shared<bool>(true) };
};

RESTAPI::RESTAPI() {
    mLoop = uWS::Loop::get();
    assert(mLoop);
}

void RESTAPI::run(ApplicationState& app) {
    uWS::App webApp;
    webApp
        .post(
            "/mqtt/*",
            [&app](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
                try {
                    std::string topic{ req->getUrl().substr(6) };
                    if(topic.empty()) {
                        res->writeStatus("400 Bad Request");
                        res->end("Invalid topic", true);
                        return;
                    }
                    std::string retainStr{ req->getQuery("retain") };
                    Retain retain = Retain::No;
                    if(retainStr.empty() || retainStr == "false") {
                        retain = Retain::No;
                    } else if(retainStr == "true") {
                        retain = Retain::Yes;
                    } else {
                        res->writeStatus("400 Bad Request");
                        res->end("Invalid retain (true or false allowed)", true);
                        return;
                    }
                    auto payloadStringView = req->getQuery("payload");
                    std::vector<uint8_t> payload{ payloadStringView.begin(), payloadStringView.end() };

                    std::string qosStr{ req->getQuery("qos") };
                    QoS qos = QoS::QoS0;
                    if(qosStr.empty() || qosStr == "0") {
                        qos = QoS::QoS0;
                    } else if(qosStr == "1") {
                        qos = QoS::QoS1;
                    } else if(qosStr == "2") {
                        qos = QoS::QoS2;
                    } else {
                        res->writeStatus("400 Bad Request");
                        res->end("Invalid QoS (0, 1 or 2 allowed)", true);
                        return;
                    }
                    app.publish(std::move(topic), std::move(payload), qos, retain);
                    res->end("ok", true);
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what(), true);
                    return;
                }
            })
        .ws<PerWebsocketClientData>(
            "/mqtt/*",
            uWS::TemplatedApp<false>::WebSocketBehavior<PerWebsocketClientData>{ .sendPingsAutomatically = true,
                                                                                 .upgrade =
                                                                                     [](auto* res, uWS::HttpRequest* req, auto* context) {
                                                                                         res->template upgrade<PerWebsocketClientData>(
                                                                                             PerWebsocketClientData{ std::string{ req->getUrl().substr(6) } }, req->getHeader("sec-websocket-key"),
                                                                                             req->getHeader("sec-websocket-protocol"), req->getHeader("sec-websocket-extensions"), context);
                                                                                     },
                                                                                 .open =
                                                                                     [this, &app](uWS::WebSocket<false, true, PerWebsocketClientData>* ws) {
                                                                                         auto userData = ws->getUserData();
                                                                                         spdlog::info("New WS subscription on: {}", userData->topic);
                                                                                         auto sub = std::make_shared<WSSubscriber>(app, *mLoop, ws);
                                                                                         app.requestChange(makeChangeRequestSubscribe(sub, std::move(userData->topic), QoS::QoS0));
                                                                                         openWSFds.emplace(ws, std::move(sub));
                                                                                     },
                                                                                 .close =
                                                                                     [this, &app](uWS::WebSocket<false, true, PerWebsocketClientData>* ws, int code, std::string_view message) {
                                                                                         auto it = openWSFds.find(ws);
                                                                                         if(it == openWSFds.end()) {
                                                                                             spdlog::warn("Close called on already closed fd!");
                                                                                             return;
                                                                                         }
                                                                                         it->second->deactivate();
                                                                                         app.requestChange(ChangeRequestUnsubscribeFromAll{ it->second });
                                                                                         openWSFds.erase(it);
                                                                                     } })
        .put(
            "/scripts/:script_name",
            [&app](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
                try {
                    std::string fullCode;
                    std::string scriptName{ req->getParameter(0) };
                    res->onData([res, scriptName, fullCode, &app](std::string_view data, bool last) mutable {
                        fullCode += data;
                        if(!last) {
                            return;
                        }
                        spdlog::info("Adding script from Web-API: {}", scriptName);
                        app.addScript(
                            scriptName, [res](auto&) { res->end("ok", true); },
                            [res](auto&, const auto& error) {
                                res->writeStatus("500 Internal Server Error");
                                res->end(error, true);
                            },
                            std::move(fullCode));
                        res->end("ok", true);
                    });
                    res->onAborted([] {});
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what(), true);
                }
            })
        .post(
            "/scripts/:script_name/activate",
            [&app](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
                try {
                    std::string scriptName{ req->getParameter(0) };
                    app.requestChange(ChangeRequestActivateScript{ std::move(scriptName) }, ApplicationState::RequestChangeMode::SYNC);
                    res->end("ok", true);
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what(), true);
                }
            })
        .post(
            "/scripts/:script_name/deactivate",
            [&app](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
                try {
                    std::string scriptName{ req->getParameter(0) };
                    app.requestChange(ChangeRequestDeactivateScript{ std::move(scriptName) }, ApplicationState::RequestChangeMode::SYNC);
                    res->end("ok", true);
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what(), true);
                }
            })
        .del(
            "/scripts/:script_name",
            [&app](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
                try {
                    std::string scriptName{ req->getParameter(0) };
                    spdlog::info("Deleting script from Web-API: {}", scriptName);
                    app.requestChange(ChangeRequestDeleteScript{ std::move(scriptName) });
                    res->end("ok", true);
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what(), true);
                }
            })
        .get(
            "/scripts",
            [&app](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
                try {
                    auto scriptsInfo = app.getScriptsInfo();
                    rapidjson::Document doc;
                    doc.SetObject();
                    for(auto& script : scriptsInfo.scripts) {
                        rapidjson::Value scriptObj;
                        scriptObj.SetObject();
                        scriptObj.AddMember("name", rapidjson::Value{ script.name.c_str(), static_cast<rapidjson::SizeType>(script.name.size()), doc.GetAllocator() }.Move(), doc.GetAllocator());
                        //scriptObj.AddMember("code", rapidjson::Value{ script.code.c_str(), static_cast<rapidjson::SizeType>(script.code.size()), doc.GetAllocator() }.Move(), doc.GetAllocator());
                        scriptObj.AddMember("active", rapidjson::Value{ script.active }.Move(), doc.GetAllocator());
                        doc.AddMember(
                            rapidjson::Value{ script.name.c_str(), static_cast<rapidjson::SizeType>(script.name.size()), doc.GetAllocator() }.Move(), std::move(scriptObj.Move()), doc.GetAllocator());
                    }
                    rapidjson::StringBuffer docStringified;
                    rapidjson::Writer<rapidjson::StringBuffer> docWriter{ docStringified };
                    doc.Accept(docWriter);
                    res->end({ docStringified.GetString(), docStringified.GetLength() }, true);
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what(), true);
                }
            })
        .get(
            "/scripts/:script_name",
            [&app](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
                try {
                    auto scriptsInfo = app.getScriptsInfo();
                    std::string scriptName{ req->getParameter(0) };
                    rapidjson::Document doc;
                    doc.SetObject();
                    bool scriptFound = false;
                    for(auto& script : scriptsInfo.scripts) {
                        if(script.name != scriptName)
                            continue;
                        doc.AddMember("name", rapidjson::Value{ script.name.c_str(), static_cast<rapidjson::SizeType>(script.name.size()), doc.GetAllocator() }.Move(), doc.GetAllocator());
                        doc.AddMember("code", rapidjson::Value{ script.code.c_str(), static_cast<rapidjson::SizeType>(script.code.size()), doc.GetAllocator() }.Move(), doc.GetAllocator());
                        doc.AddMember("active", rapidjson::Value{ script.active }.Move(), doc.GetAllocator());
                        scriptFound = true;
                    }
                    if(!scriptFound) {
                        res->writeStatus("404 Not Found");
                        res->end("Script not found", true);
                        return;
                    }
                    rapidjson::StringBuffer docStringified;
                    rapidjson::Writer<rapidjson::StringBuffer> docWriter{ docStringified };
                    doc.Accept(docWriter);
                    res->end({ docStringified.GetString(), docStringified.GetLength() }, true);
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what(), true);
                }
            })
        .get(
            "/script_dev_files",
            [&app](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
                try {
                    static std::string body;
                    if(body.empty()) {
                        static rapidjson::Document doc;
                        doc.SetObject();
                        doc.AddMember(rapidjson::StringRef("quickjs.h"), rapidjson::StringRef((const char*)quickjs_h, quickjs_h_len), doc.GetAllocator());
                        rapidjson::StringBuffer docStringified;
                        rapidjson::Writer<rapidjson::StringBuffer> docWriter{ docStringified };
                        doc.Accept(docWriter);
                        body.append(docStringified.GetString(), docStringified.GetLength());
                        spdlog::info("Generated script dev files json doc");
                    }
                    res->end(body, true);
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what(), true);
                }
            })
        .get(
            "/statistics",
            [&app](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
                try {
                    rapidjson::Document doc;
                    doc.SetObject();
                    auto stats = app.getAnalysisResults();

                    doc.AddMember(rapidjson::StringRef("total_msg_count"), rapidjson::Value{ stats.totalPacketCount }, doc.GetAllocator());
                    doc.AddMember(rapidjson::StringRef("current_sleep_level"), rapidjson::StringRef(workerThreadSleepLevelToString(stats.currentSleepLevel)), doc.GetAllocator());
                    doc.AddMember(rapidjson::StringRef("app_state_queue_depth"), rapidjson::Value{ stats.appStateQueueDepth }, doc.GetAllocator());
                    doc.AddMember(rapidjson::StringRef("retained_msg_count"), rapidjson::Value{ stats.retainedMsgCount }, doc.GetAllocator());
                    doc.AddMember(rapidjson::StringRef("retained_msg_size_sum"), rapidjson::Value{ stats.retainedMsgCummulativeSize }, doc.GetAllocator());
                    doc.AddMember(rapidjson::StringRef("uptime_seconds"), rapidjson::Value{ stats.uptimeSeconds }, doc.GetAllocator());
                    {
                        rapidjson::Value subs;
                        subs.SetObject();
                        for(auto& sub : stats.activeSubscriptions) {
                            subs.AddMember(
                                rapidjson::Value{ sub.first.c_str(), static_cast<rapidjson::SizeType>(sub.first.size()), doc.GetAllocator() }, rapidjson::Value{ sub.second }, doc.GetAllocator());
                        }
                        doc.AddMember(rapidjson::StringRef("active_subscriptions"), std::move(subs.Move()), doc.GetAllocator());
                    }
                    {
                        rapidjson::Value sleepLevelCounts;
                        sleepLevelCounts.SetObject();
                        stats.sleepLevelSampleCounts.erase(--stats.sleepLevelSampleCounts.end());
                        for(auto& t : stats.sleepLevelSampleCounts) {
                            rapidjson::Value tobj;
                            tobj.SetObject();
                            for(int i = 0; i < static_cast<int>(WorkerThreadSleepLevel::$COUNT); ++i) {
                                tobj.AddMember(rapidjson::StringRef(workerThreadSleepLevelToString(static_cast<WorkerThreadSleepLevel>(i))), rapidjson::Value{ t.samples.at(i) }, doc.GetAllocator());
                            }
                            auto str = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(t.timestamp.time_since_epoch()).count());
                            sleepLevelCounts.AddMember(rapidjson::Value{ str.c_str(), static_cast<rapidjson::SizeType>(str.size()), doc.GetAllocator() }, std::move(tobj.Move()), doc.GetAllocator());
                        }
                        doc.AddMember(rapidjson::StringRef("sleep_level_sample_counts"), std::move(sleepLevelCounts.Move()), doc.GetAllocator());
                    }
                    {
                        rapidjson::Value topics;
                        topics.SetObject();
                        for(auto& topic : stats.topics) {
                            rapidjson::Value topicJson;
                            topicJson.SetObject();
                            topic.second.erase(--topic.second.end());
                            for(auto& ti : topic.second) {
                                rapidjson::Value tiJson;
                                tiJson.SetObject();
                                tiJson.AddMember(rapidjson::StringRef("msg_size_sum"), rapidjson::Value{ ti.cummulativePacketSize }, doc.GetAllocator());
                                tiJson.AddMember(rapidjson::StringRef("msg_count"), rapidjson::Value{ ti.packetCount }, doc.GetAllocator());
                                tiJson.AddMember(rapidjson::StringRef("qos_0_packet_count"), rapidjson::Value{ ti.qosPacketCounts.at(0) }, doc.GetAllocator());
                                tiJson.AddMember(rapidjson::StringRef("qos_1_packet_count"), rapidjson::Value{ ti.qosPacketCounts.at(1) }, doc.GetAllocator());
                                tiJson.AddMember(rapidjson::StringRef("qos_2_packet_count"), rapidjson::Value{ ti.qosPacketCounts.at(2) }, doc.GetAllocator());

                                auto str = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(ti.timestamp.time_since_epoch()).count());
                                topicJson.AddMember(rapidjson::Value{ str.c_str(), static_cast<rapidjson::SizeType>(str.size()), doc.GetAllocator() }, std::move(tiJson.Move()), doc.GetAllocator());
                            }
                            topics.AddMember(
                                rapidjson::Value{ topic.first.c_str(), static_cast<rapidjson::SizeType>(topic.first.size()), doc.GetAllocator() }, std::move(topicJson.Move()), doc.GetAllocator());
                        }
                        doc.AddMember(rapidjson::StringRef("topics"), std::move(topics.Move()), doc.GetAllocator());
                    }
                    {
                        auto addHistogram = [&](std::vector<AnalysisResults::TimeInfo>& data, const char* name) {
                            rapidjson::Value obj;
                            obj.SetObject();
                            data.erase(--data.end());
                            for(auto& interval : data) {
                                rapidjson::Value intervalObj;
                                intervalObj.SetObject();
                                intervalObj.AddMember(rapidjson::StringRef("msg_count"), rapidjson::Value{ interval.packetCount }, doc.GetAllocator());
                                intervalObj.AddMember(rapidjson::StringRef("msg_size_sum"), rapidjson::Value{ interval.cummulativePacketSize }, doc.GetAllocator());
                                auto str = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(interval.timestamp.time_since_epoch()).count());
                                obj.AddMember(rapidjson::Value{ str.c_str(), static_cast<rapidjson::SizeType>(str.size()), doc.GetAllocator() }, std::move(intervalObj.Move()), doc.GetAllocator());
                            }
                            doc.AddMember(rapidjson::StringRef(name), std::move(obj.Move()), doc.GetAllocator());
                        };
                        addHistogram(stats.packetsPerSecond, "msg_per_second");
                        addHistogram(stats.packetsPerMinute, "msg_per_minute");
                    }

                    rapidjson::StringBuffer docStringified;
                    rapidjson::Writer<rapidjson::StringBuffer> docWriter{ docStringified };
                    doc.Accept(docWriter);
                    std::string body{ docStringified.GetString(), docStringified.GetLength() };

                    res->end(body, true);
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what(), true);
                }
            })
        .get(
            "/*",
            [](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
                try {
                    auto path = req->getUrl().substr(1);
                    spdlog::info("Web request for {}", path);
                    std::string fullFilePath = "webui/" + std::string{ path };
                    std::ifstream file{ fullFilePath };
                    if(!file) {
                        res->writeStatus("404 Not Found");
                        res->end("Not found", true);
                        return;
                    }
                    auto extension = util::getFileExtension(fullFilePath);
                    if(extension == ".js") {
                        res->writeHeader("Content-Type", "application/javascript");
                    } else if(extension == ".html") {
                        res->writeHeader("Content-Type", "text/html");
                    }
                    std::string contents(std::istreambuf_iterator<char>(file), {});
                    res->end(contents, true);
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what(), true);
                }
            })
        .listen(1884, [this](auto* listenSocket) {
            mListenSocket.store(listenSocket);
            if(listenSocket) {
                spdlog::info("HTTP Server started");
            }
        });

    webApp.run();
}

void RESTAPI::abort() {
    /* uWebSockets (the web library we're using) is a bit weird in that it doesn't have an exit or close function, but instead just exits
     * it's main loop when all active connections are dead. To speed that process up, we just close all connections manually here. If there is
     * data still in transit, that's not our problem, network connection can drop at any point anyways.
     */
    // close all open ws connections
    mLoop->defer([this] {
        if(mListenSocket) {
            us_listen_socket_close(false, mListenSocket.load());
            mListenSocket = nullptr;
        }
        // use a copy because close modifies openWSFds
        auto cpy = openWSFds;
        for(auto fd : cpy) {
            fd.first->close();
        }
    });
}
}