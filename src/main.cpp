#include "spdlog/spdlog.h"
#include "spdlog/sinks/base_sink.h"

#include "ApplicationState.hpp"
#include "BigString.hpp"
#include "BigVector.hpp"
#include "scripting/ScriptContainerJS.hpp"
#include "scripting/ScriptContainerManager.hpp"
#include "TcpServer.hpp"
#include <csignal>

#include "App.h"
#include "HttpResponse.h"

using namespace nioev;

std::atomic<TcpServer*> gTcpServer = nullptr;
std::atomic<struct us_listen_socket_t*> gListenSocket = nullptr;

constexpr const char* LOG_PATTERN = "[%Y-%m-%d %H:%M:%S.%e] %^[%-7l]%$ [%-15N] %v";

void onExitSignal(int) {
    if(gTcpServer) {
        gTcpServer.load()->requestStop();
        gTcpServer = nullptr;
    }
    if(gListenSocket) {
        us_listen_socket_close(false, gListenSocket);
        gListenSocket = nullptr;
    }
}

class LogSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    LogSink(ApplicationState& app)
    : mApp(app) {
        set_pattern_(LOG_PATTERN);
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        spdlog::sinks::base_sink<std::mutex>::formatter_->format(msg, formatted);
        assert(formatted.size() > 0);
        std::vector<uint8_t> formattedBuffer((uint8_t*)formatted.begin(), (uint8_t*)formatted.end() - 1);
        mApp.requestChange(ChangeRequestPublish{LOG_TOPIC, std::move(formattedBuffer), QoS::QoS0, Retain::No});
    }

    void flush_() override {
        std::cout << std::flush;
    }
    ApplicationState& mApp;
};

int main() {
    signal(SIGUSR1, [](int) {});
    signal(SIGINT, onExitSignal);
    signal(SIGTERM, onExitSignal);

    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern(LOG_PATTERN);

    ApplicationState app;
    spdlog::default_logger()->sinks().push_back(std::make_shared<LogSink>(app));


    TcpServer server{ 1883, app };
    gTcpServer = &server;
    spdlog::info("MQTT Broker started");

    uWS::App{}
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
                            std::string{scriptName},
                            [res](auto&) {
                                res->end("ok");
                            },
                            [res](auto&, const auto& error) {
                                res->writeStatus("500 Internal Server Error");
                                res->end(error);
                            },
                            std::move(fullCode));
                    });
                    res->onAborted([]{});
                } catch(std::exception& e) {
                    res->writeStatus("500 Internal Server Error");
                    res->end(e.what());
                    return;
                }
            })
        .listen(
            1884,
            [](auto* listenSocket) {
                gListenSocket = listenSocket;
                if(listenSocket) {
                    spdlog::info("HTTP Server started");
                }
            })
        .run();

    server.join();
}