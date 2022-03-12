#include "spdlog/details/null_mutex.h"
#include "spdlog/spdlog.h"

#include "ApplicationState.hpp"
#include "BigString.hpp"
#include "BigVector.hpp"
#include "scripting/ScriptContainerJS.hpp"
#include "scripting/ScriptContainerManager.hpp"
#include "TcpServer.hpp"
#include <csignal>

#include "scripting/NativeLibrary.hpp"
#include "REST.hpp"
#include "Util.hpp"


using namespace nioev;

std::atomic<TcpServer*> gTcpServer = nullptr;


int main() {

    // block exit signals in all threads
    sigset_t exitSignals;
    sigemptyset(&exitSignals);
    sigaddset(&exitSignals, SIGINT);
    sigaddset(&exitSignals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &exitSignals, nullptr);
    signal(SIGUSR1, [](int) {});

    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern(nioev::util::LOG_PATTERN);
    ApplicationState app;

    RESTAPI rest;

    std::thread signalHandler{ [&] {
        pthread_setname_np(pthread_self(), "signal-handler");
        int receivedSignal = 0;
        if(sigwait(&exitSignals, &receivedSignal) > 0) {
            perror("sigwait failed");
        }
        if(gTcpServer) {
            gTcpServer.load()->requestStop();
            gTcpServer = nullptr;
        }
        rest.abort();
    } };


    TcpServer server{ 1883, app };
    gTcpServer = &server;
    spdlog::info("MQTT Broker started");

    pthread_setname_np(pthread_self(), "uwebsockets");
    rest.run(app);
    spdlog::info("uWebSockets shutdown");

    server.join();
    signalHandler.join();
}