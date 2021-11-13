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
        []() { spdlog::info("Success!"); },
        [](const auto& error) { spdlog::error("{}", error); },
        std::string{ R"--(
i = 0

function run(args) {
    return [
        {
            type: 'publish',
            topic: 'hello',
            payloadStr: "Index: " + (i++),
            qos: 0,
            retain: false
        },
        {
            type: 'unsubscribe',
            topic: 'scriptTest'
        }];
}

initArgs = {}
initArgs.runType = 'async'
initArgs.actions = [
    {
        type: 'publish',
        topic: 'helloInitial',
        payloadStr: "test",
        qos: 0,
        retain: false
    },
    {
        type: 'subscribe',
        topic: 'scriptTest'
    }
]
initArgs)--" });
    //clientManager.deleteScript("test");

    TcpServer server{ 1883 };
    spdlog::info("MQTT TcpServer started");
    server.loop(clientManager);
}