#include "spdlog/sinks/base_sink.h"
#include "spdlog/details/null_mutex.h"
#include "spdlog/spdlog.h"

#include "ApplicationState.hpp"
#include "BigString.hpp"
#include "BigVector.hpp"
#include "scripting/ScriptContainerJS.hpp"
#include "scripting/ScriptContainerManager.hpp"
#include "TcpServer.hpp"
#include <csignal>

#include "App.h"
#include "HttpResponse.h"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

using namespace nioev;

std::atomic<TcpServer*> gTcpServer = nullptr;
std::atomic<struct us_listen_socket_t*> gListenSocket = nullptr;

constexpr const char* LOG_PATTERN = "[%Y-%m-%d %H:%M:%S.%e] %^[%-7l]%$ [%-15N] %v";


class LogSink : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
public:
    LogSink(ApplicationState& app) : mApp(app) {
        set_pattern_(LOG_PATTERN);
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        spdlog::sinks::base_sink<spdlog::details::null_mutex>::formatter_->format(msg, formatted);
        assert(formatted.size() > 0);
        std::vector<uint8_t> formattedBuffer((uint8_t*)formatted.begin(), (uint8_t*)formatted.end() - 1);
        mApp.publishAsync(AsyncPublishData{ LOG_TOPIC, std::move(formattedBuffer), QoS::QoS0, Retain::No });
    }

    void flush_() override {
    }
    ApplicationState& mApp;
};

struct PerWebsocketClientData {
    std::string topic;
};

int main() {

    // block exit signals in all threads
    sigset_t exitSignals;
    sigemptyset(&exitSignals);
    sigaddset(&exitSignals, SIGINT);
    sigaddset(&exitSignals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &exitSignals, nullptr);
    signal(SIGUSR1, [](int) {});

    uWS::App webApp;
    auto loop = uWS::Loop::get();
    assert(loop);
    std::unordered_set<uWS::WebSocket<false, true, PerWebsocketClientData>*> openWSFds;

    std::thread signalHandler{
        [&] {
            pthread_setname_np(pthread_self(), "signal-handler");
            int receivedSignal = 0;
            if(sigwait(&exitSignals, &receivedSignal) > 0) {
                perror("sigwait failed");
            }
            if(gTcpServer) {
                gTcpServer.load()->requestStop();
                gTcpServer = nullptr;
            }
            /* uWebSockets (the web library we're using) is a bit weird in that it doesn't have an exit or close function, but instead just exits
             * it's main loop when all active connections are dead. To speed that process up, we just close all connections manually here. If there is
             * data still in transit, that's not our problem, network connection can drop at any point anyways.
             */
            if(gListenSocket) {
                us_listen_socket_close(false, gListenSocket);
                gListenSocket = nullptr;
            }
            // close all open ws connections
            loop->defer([&openWSFds] {
                // use a copy because close modifies openWSFds
                auto cpy = std::move(openWSFds);
                openWSFds = {};
                for(auto* fd: cpy) {
                    fd->close();
                }
            });
        }
    };
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern(LOG_PATTERN);
    ApplicationState app;
    spdlog::default_logger()->sinks().push_back(std::make_shared<LogSink>(app));

    TcpServer server{ 1883, app };
    gTcpServer = &server;
    spdlog::info("MQTT Broker started");



    webApp
        .post(
            "/mqtt/*",
            [&app](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
                try {
                    std::string topic{ req->getUrl().substr(6) };
                    if(topic.empty()) {
                        res->writeStatus("400 Bad Request");
                        res->end("Invalid topic");
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
                        res->end("Invalid retain (true or false allowed)");
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
                        res->end("Invalid QoS (0, 1 or 2 allowed)");
                        return;
                    }
                    app.publish(std::move(topic), std::move(payload), qos, retain);
                    res->end("ok");
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what());
                    return;
                }
            })
        .ws<PerWebsocketClientData>(
            "/mqtt/*",
            uWS::TemplatedApp<false>::WebSocketBehavior<PerWebsocketClientData>{
                .sendPingsAutomatically = true,
                .upgrade =
                    [](auto* res, uWS::HttpRequest* req, auto* context) {
                        res->template upgrade<PerWebsocketClientData>(
                            PerWebsocketClientData{ std::string{ req->getUrl().substr(6) } }, req->getHeader("sec-websocket-key"),
                            req->getHeader("sec-websocket-protocol"), req->getHeader("sec-websocket-extensions"), context);
                    },
                .open =
                    [&openWSFds](uWS::WebSocket<false, true, PerWebsocketClientData>* ws) {
                        auto userData = ws->getUserData();
                        spdlog::info("New WS subscription on: {}", userData->topic);
                        ws->subscribe(userData->topic);
                        openWSFds.emplace(ws);
                    },
                .close =
                    [&openWSFds](uWS::WebSocket<false, true, PerWebsocketClientData>* ws, int code, std::string_view message) {
                        openWSFds.erase(ws);
                    }})
        .put(
            "/scripts/:script_name",
            [&app](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
                try {
                    std::string fullCode;
                    res->onData([res, req, fullCode, &app](std::string_view data, bool last) mutable {
                        fullCode += data;
                        if(!last) {
                            return;
                        }
                        auto scriptName = req->getParameter(0);
                        auto language = req->getQuery("lang");
                        if(language != "js") {
                            res->writeStatus("400 Bad Request");
                            res->end("Invalid language - please specify lang=js");
                            return;
                        }
                        spdlog::info("Adding script from Web-API: {}", scriptName);
                        app.addScript<ScriptContainerJS>(
                            std::string{ scriptName }, [res](auto&) { res->end("ok"); },
                            [res](auto&, const auto& error) {
                                res->writeStatus("500 Internal Server Error");
                                res->end(error);
                            },
                            std::move(fullCode));
                    });
                    res->onAborted([] {});
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what());
                }
            })
        .get(
            "/scripts",
            [&app](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
                try {
                    auto scriptsInfo = app.getScriptsInfo();
                    rapidjson::Document doc;
                    doc.SetArray();
                    for(auto& script : scriptsInfo.scripts) {
                        rapidjson::Value scriptObj;
                        scriptObj.SetObject();
                        scriptObj.AddMember("name", rapidjson::Value{script.name.c_str(), static_cast<rapidjson::SizeType>(script.name.size()), doc.GetAllocator()}.Move(), doc.GetAllocator());
                        scriptObj.AddMember("code", rapidjson::Value{script.code.c_str(), static_cast<rapidjson::SizeType>(script.code.size()), doc.GetAllocator()}.Move(), doc.GetAllocator());
                        doc.PushBack(std::move(scriptObj.Move()), doc.GetAllocator());
                    }
                    rapidjson::StringBuffer docStringified;
                    rapidjson::Writer<rapidjson::StringBuffer> docWriter{docStringified};
                    doc.Accept(docWriter);
                    res->end({docStringified.GetString(), docStringified.GetLength()});
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what());
                }
            })
        .listen(1884, [](auto* listenSocket) {
            gListenSocket = listenSocket;
            if(listenSocket) {
                spdlog::info("HTTP Server started");
            }
        });

    class WSSubscriber : public nioev::Subscriber {
    public:
        WSSubscriber(uWS::App& webApp, uWS::Loop& loop) : mWebApp(webApp), mLoop(loop) { }
        void publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained) override {
            mLoop.defer([this, topic, payload] {
                // TODO use our publish-subscribe implementation instead, because the one used by the library isn't actually mqtt spec compliant
                mWebApp.publish(topic, std::string_view{ (const char*)payload.data(), payload.size() }, uWS::BINARY, false);
            });
        }

    private:
        uWS::App& mWebApp;
        uWS::Loop& mLoop;
    };

    // TODO this subscription takes about 30% of our processing time! -> Remove!
    app.requestChange(
        ChangeRequestSubscribe{ std::make_shared<WSSubscriber>(webApp, *loop), "#", { "#" }, nioev::SubscriptionType::OMNI, QoS::QoS0 });

    webApp.run();

    server.join();
    signalHandler.join();
}