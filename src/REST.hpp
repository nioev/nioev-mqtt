#pragma once

#include "Forward.hpp"
#include <unordered_map>
#include <memory>
#include <atomic>


struct us_listen_socket_t;

namespace uWS {

template <bool SSL, bool isServer, typename USERDATA>
struct WebSocket;

struct Loop;
}

namespace nioev::mqtt {

struct PerWebsocketClientData;
struct WSSubscriber;

class RESTAPI {
public:
    RESTAPI();
    void run(ApplicationState& app);
    void abort();
private:
    uWS::Loop* mLoop{ nullptr};
    std::atomic<us_listen_socket_t*> mListenSocket{nullptr};
    std::unordered_map<uWS::WebSocket<false, true, PerWebsocketClientData>*, std::shared_ptr<WSSubscriber>> openWSFds;
};

}