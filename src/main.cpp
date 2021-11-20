#include "spdlog/spdlog.h"

#include "Application.hpp"
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

int main() {
    signal(SIGUSR1, [](int) {});
    signal(SIGINT, onExitSignal);
    signal(SIGTERM, onExitSignal);

    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] %^[%-5l]%$ [%-15N] %v");

    Application app;
    app.addScript<ScriptContainerJS>(
        "test", [](auto&) { spdlog::info("Successfully added testscript!"); }, [](auto&, const auto& error) { spdlog::error("{}", error); },
        std::string{ R"--(
i = 0

function run(args) {
    if(args.topic === "night_mode") {
        let payload = "";
        if(args.payloadStr == "0") {
            payload = "on";
        } else if(args.payloadStr == "1") {
            payload = "off";
        }
        if(payload != "") {
            return {
                actions: [{
                    type: 'publish',
                    topic: 'shellies/shellyplug-s-DD7977/relay/0/command',
                    payloadStr: payload,
                    qos: 0,
                    retain: true
                }],
            }
        }
    } else if(args.topic == "random") {
        return {
            syncAction: "abortPublish",
            actions: [{
                type: 'publish',
                topic: 'random2',
                payloadStr: String(Math.random()),
                qos: 0,
                retain: true
            }],
        }
    } else if(args.topic == "testI") {
        return {
            actions: [{
                type: 'publish',
                topic: 'testO',
                payloadBytes: args.payloadBytes,
                qos: 0,
                retain: true
            }],
        }
    }
    return {};
}

initArgs = {}
initArgs.runType = 'async'
initArgs.actions = [
    {
        type: 'subscribe',
        topic: 'night_mode'
    },
    {
        type: 'subscribe',
        topic: 'random'
    },
    {
        type: 'subscribe',
        topic: 'testI'
    }
]
initArgs)--" });
    // clientManager.deleteScript("test");

    TcpServer server{ 1883, app };
    gTcpServer = &server;
    spdlog::info("MQTT TcpServer started");

    uWS::App{}
        .get(
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
        .listen(
            8080,
            [](auto* listenSocket) {
                gListenSocket = listenSocket;
                if(listenSocket) {
                    spdlog::info("HTTP Server started");
                }
            })
        .run();

    server.join();
}