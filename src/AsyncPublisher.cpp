#include "AsyncPublisher.hpp"
#include "ApplicationState.hpp"

namespace nioev {

nioev::AsyncPublisher::AsyncPublisher(ApplicationState& app)
: mApp(app), mThread([this]{secondThreadFunc();}) {

}
AsyncPublisher::~AsyncPublisher() {
    std::unique_lock<std::mutex> lock{mQueueMutex};
    mShouldRun = false;
    lock.unlock();
    mQueueCV.notify_all();
    mThread.join();
}
void AsyncPublisher::publishAsync(AsyncPublishData&& publish) {
    std::unique_lock<std::mutex> lock{mQueueMutex};
    mQueue.emplace(std::move(publish));
    lock.unlock();
    mQueueCV.notify_all();
}
void AsyncPublisher::secondThreadFunc() {
    pthread_setname_np(pthread_self(), "async-pub");
    std::unique_lock<std::mutex> lock{mQueueMutex};
    while(true) {
        mQueueCV.wait(lock);
        if(!mShouldRun)
            return;
        while(!mQueue.empty()) {
            auto pub = std::move(mQueue.front());
            lock.unlock();
            mApp.publish(std::move(pub.topic), std::move(pub.payload), pub.qos, pub.retain);
            lock.lock();
            mQueue.pop();
        }

    }
}

}
