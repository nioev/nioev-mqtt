#include "spdlog/spdlog.h"

#include "Application.hpp"
#include "BigString.hpp"
#include "BigVector.hpp"
#include "scripting/ScriptContainerJS.hpp"
#include "scripting/ScriptContainerManager.hpp"
#include "TcpServer.hpp"
#include <csignal>

using namespace nioev;

int main() {
    signal(SIGUSR1, [](int) {});
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] %^[%-5l]%$ [%-15N] %v");

    {
       BigString<16> test{"Hallo schöne Welt"};
       spdlog::info("Test is: {}, Optimized: {}", test.c_str(), test.isShortOptimized());
    }
    {
        int data[5] = { 0 };
        BigVector<int, 4> test{data, 5};
        for(int i = 0; i < 5; ++i) {
            spdlog::info("{}: {}", i, test[i]);
        }
    }

    Application clientManager;

    clientManager.addScript<ScriptContainerJS>(
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

    TcpServer server{ 1883 };
    spdlog::info("MQTT TcpServer started");
    server.loop(clientManager);
}