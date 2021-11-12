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

    ScriptContainerManager mScripts;
    mScripts.addScript<ScriptContainerJS>(
        "test",
        ScriptInitOutputArgs{ .error = [](const auto& error) { spdlog::error("Script error: {}", error); },
                              .success = [](const auto& initArgs) { spdlog::info("Success!"); } },
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
initArgs)--" });

    mScripts.runScript(
        "test", ScriptInputArgs{ ScriptRunArgsMqttMessage{ .topic = "abc", .payload = { 'd', 'e', 'f' } } },
        ScriptOutputArgs{
            .publish = [](const auto& topic, const auto&, auto, auto) {
                spdlog::info("Publishing from script on topic {}", topic);
            },
            .subscribe = [](const auto&) {

            },
            .unsubscribe = [](const auto&) {

            },
            .error =
                [](const auto& error) {
                    spdlog::error("Script error: {}", error);
                },
            .syncAction = [](auto) {

            },

        });
    sleep(1);
    mScripts.deleteScript("test");

    Application clientManager;
    TcpServer server{ 1883 };
    spdlog::info("MQTT TcpServer started");
    server.loop(clientManager);
}