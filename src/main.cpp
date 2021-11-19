#include "spdlog/spdlog.h"

#include "Application.hpp"
#include "BigString.hpp"
#include "BigVector.hpp"
#include "scripting/ScriptContainerJS.hpp"
#include "scripting/ScriptContainerManager.hpp"
#include "TcpServer.hpp"
#include <csignal>

using namespace nioev;


std::atomic<TcpServer*> gTcpServer = nullptr;

void onExitSignal(int) {
    if(gTcpServer) {
        gTcpServer.load()->requestStop();
        gTcpServer = nullptr;
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
        "test",
        [](auto&) { spdlog::info("Successfully added testscript!"); },
        [](auto&, const auto& error) { spdlog::error("{}", error); },
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
    //clientManager.deleteScript("test");

    TcpServer server{ 1883, app };
    gTcpServer = &server;
    spdlog::info("MQTT TcpServer started");
    server.join();
}