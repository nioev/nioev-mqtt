#include "AsyncPublisher.hpp"
#include "ApplicationState.hpp"

namespace nioev {

nioev::AsyncPublisher::AsyncPublisher(ApplicationState& app)
: GenServer<AsyncPublishData>("async-pub") , mApp(app) {

}
void AsyncPublisher::handleTask(AsyncPublishData&& pub) {
    mApp.publish(std::move(pub.topic), std::move(pub.payload), pub.qos, pub.retain);
}

}
