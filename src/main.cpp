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
function run(args) {
    return [{
        type: 'publish',
        topic: 'hello',
        payloadStr: "test",
        qos: 0,
        retain: false
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
    sleep(1);
    clientManager.deleteScript("test");

    TcpServer server{ 1883 };
    spdlog::info("MQTT TcpServer started");
    server.loop(clientManager);
}