#include "ScriptActionPerformer.hpp"
#include "../ApplicationState.hpp"

namespace nioev {

ScriptActionPerformer::ScriptActionPerformer(ApplicationState& app) : mApp(app), mActionsPerformerThread([this] { actionsPerformerThreadFunc(); }) { }
ScriptActionPerformer::~ScriptActionPerformer() {
    mShouldRun = false;
    mActionsCV.notify_all();
    mActionsPerformerThread.join();
}
void ScriptActionPerformer::enqueueAction(ScriptAction&& action) {
    std::unique_lock<std::mutex> lock{ mActionsMutex };
    mActions.emplace(std::move(action));
    lock.unlock();
    mActionsCV.notify_all();
}
void ScriptActionPerformer::actionsPerformerThreadFunc() {
    pthread_setname_np(pthread_self(), "script-action");
    while(mShouldRun) {
        std::unique_lock<std::mutex> lock{ mActionsMutex };
        while(mActions.empty()) {
            mActionsCV.wait(lock);
            if(!mShouldRun) {
                return;
            }
        }
        while(!mActions.empty() && mShouldRun) {
            auto action = std::move(mActions.front());
            mActions.pop();
            lock.unlock();

            std::visit(
                util::overloaded{ [this](ScriptActionPublish& publish) {
                               //mApp.publish(std::move(publish.topic), std::move(publish.payload), publish.qos, publish.retain);
                           },
                            [this](ScriptActionSubscribe& arg) {
                                      //mApp.addSubscription(std::move(arg.scriptName), std::move(arg.topic));
                                      },
                            [this](ScriptActionUnsubscribe& arg) {
                                      //mApp.deleteSubscription(std::move(arg.scriptName), std::move(arg.topic));
                                  },
                            [this](ScriptActionListen& arg) {
                                //mApp.scriptTcpListen(std::move(arg.scriptName), std::move(arg.listenIdentifier), arg.sendCompression, arg.recvCompression);
                            },
                            [this](ScriptActionSendToClient& arg) {
                                //mApp.scriptTcpSendToClient(std::move(arg.scriptName), arg.fd, std::move(arg.data));
                            } },
                action);

            lock.lock();
        }
    }
}

}