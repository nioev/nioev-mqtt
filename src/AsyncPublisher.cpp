#include "AsyncPublisher.hpp"
#include "ApplicationState.hpp"

namespace nioev::mqtt {

AsyncPublisher::AsyncPublisher(ApplicationState& app)
: GenServer<MQTTPacket>("async-pub") , mApp(app) {
    startThread();
}
void AsyncPublisher::handleTask(MQTTPacket&& pub) {
    mApp.publish(std::move(pub.topic), std::move(pub.payload), pub.qos, pub.retain, pub.properties);
}

}
