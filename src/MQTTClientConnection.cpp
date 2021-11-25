#include "MQTTClientConnection.hpp"
#include "MQTTPersistentState.hpp"

namespace nioev {

std::string MQTTClientConnection::getClientId() {
    std::lock_guard lock{mRemaingingMutex};
    return mPersistentState->clientId;
}

}