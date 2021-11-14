#include "spdlog/spdlog.h"

#include "Application.hpp"
#include "scripting/ScriptContainerJS.hpp"
#include "scripting/ScriptContainerManager.hpp"
#include "TcpServer.hpp"
#include <csignal>

using namespace nioev;

int main() {
    signal(SIGUSR1, [](int) {});
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] %^[%-5l]%$ [%-15N] %v");

    Application clientManager;

    clientManager.addScript<ScriptContainerJS>(
        "test",
        []() { spdlog::info("Successfully added testscript!"); },
        [](const auto& error) { spdlog::error("{}", error); },
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
    }
    return {};
}

initArgs = {}
initArgs.runType = 'sync'
initArgs.actions = [
    {
        type: 'subscribe',
        topic: 'night_mode'
    },
    {
        type: 'subscribe',
        topic: 'random'
    }
]
initArgs)--" });
    //clientManager.deleteScript("test");

    TcpServer server{ 1883 };
    spdlog::info("MQTT TcpServer started");
    server.loop(clientManager);
}