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
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] %^[%-7l]%$ [%-15N] %v");

    ApplicationState app;
    /*app.addScript<ScriptContainerJS>(
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
            console.log("Changing shelly plug");
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
initArgs)--" });*/
    // clientManager.deleteScript("test");

    /*app.addScript<ScriptContainerJS>(
        "test", [](auto&) { spdlog::info("Successfully added testscript!"); }, [](auto&, const auto& error) { spdlog::error("{}", error); },
        std::string{ R"--(
let s = new Set()

let t = 0

function run(args) {
    if(args.type == "publish" && args.topic == 'sbcs/ledmatrix-pico/matrix') {
        t += 0.1;
        a = []
        buffer = []
        for(let y = 0; y < 32; ++y) {
            for(let x = 0; x < 64; ++x) {
                buffer.push(Math.sin(x / 30.0 + y / 30.0 + t) * 120 + 120);
                buffer.push(Math.sin(x / 30.0 + y / 30.0 + Math.PI * 2 / 3 + t) * 120 + 120);
                buffer.push(Math.sin(x / 30.0 + y / 30.0 + Math.PI * 2 / 3 * 2 + t) * 120 + 120);
            }
        }

        bufferBytes = Uint8Array.from(buffer)
        for (let item of s) {
            a.push({
                type: 'tcp_send',
                fd: item,
                payloadBytes: bufferBytes
            })
        }
        return {
            actions: a
        }
    } else if(args.type == "tcp_new_client") {
        s.add(args.fd);
    } else if(args.type == "tcp_delete_client") {
        s.delete(args.fd);
    }
    return {};
}

initArgs = {}
initArgs.runType = 'async'
initArgs.actions = [
    {
        type: 'tcp_listen',
        identifier: 'SIMPLIFIED-SBC-MQTT-CLIENT-PICO-MATRIX',
        send_compression: 'zstd',
        recv_compression: 'none'
    },
    {
        type: 'subscribe',
        topic: 'sbcs/ledmatrix-pico/matrix'
    },
    {
        type: 'publish',
        topic: 'sbcs/ledmatrix-pico/width',
        payloadStr: '64',
        retain: true
    },
    {
        type: 'publish',
        topic: 'sbcs/ledmatrix-pico/height',
        payloadStr: '32',
        retain: true
    }
]
initArgs)--" });*/

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