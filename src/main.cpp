#include "spdlog/details/null_mutex.h"
#include "spdlog/sinks/base_sink.h"
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


using namespace nioev;

std::atomic<TcpServer*> gTcpServer = nullptr;

constexpr const char* LOG_PATTERN = "[%Y-%m-%d %H:%M:%S.%e] %^[%-7l]%$ [%-15N] %v";

class LogSink : public spdlog::sinks::base_sink<spdlog::details::null_mutex> {
public:
    LogSink(ApplicationState& app) : mApp(app) {
        set_pattern_(LOG_PATTERN);
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        spdlog::sinks::base_sink<spdlog::details::null_mutex>::formatter_->format(msg, formatted);
        assert(formatted.size() > 0);
        std::vector<uint8_t> formattedBuffer((uint8_t*)formatted.begin(), (uint8_t*)formatted.end() - 1);
        mApp.publishAsync(AsyncPublishData{ LOG_TOPIC, std::move(formattedBuffer), QoS::QoS0, Retain::No });
    }

    void flush_() override { }
    ApplicationState& mApp;
};

int main() {

    // block exit signals in all threads
    sigset_t exitSignals;
    sigemptyset(&exitSignals);
    sigaddset(&exitSignals, SIGINT);
    sigaddset(&exitSignals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &exitSignals, nullptr);
    signal(SIGUSR1, [](int) {});

    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern(LOG_PATTERN);
    ApplicationState app;
    spdlog::default_logger()->sinks().push_back(std::make_shared<LogSink>(app));

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