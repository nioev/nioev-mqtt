#include "SenderThreadManager.hpp"
#include <sys/epoll.h>
#include <csignal>


#include "MQTTClientConnection.hpp"

namespace nioev {

SenderThreadManager::SenderThreadManager(SenderThreadManagerExternalBridgeInterface& bridge, uint threadCount)
: mBridge(bridge) {
    for(uint i = 0; i< threadCount; ++i) {
        mSenderThreads.emplace_back([this, i] {
            std::string threadName = "S-" + std::to_string(i);
            pthread_setname_np(pthread_self(), threadName.c_str());
            senderThreadFunction();
        });
    }
}

void SenderThreadManager::addClientConnection(MQTTClientConnection& conn) {
    epoll_event ev = { 0 };
    ev.data.fd = conn.getTcpClient().getFd();
    ev.events = EPOLLET | EPOLLOUT | EPOLLEXCLUSIVE;
    if(epoll_ctl(mEpollFd, EPOLL_CTL_ADD, conn.getTcpClient().getFd(), &ev) < 0) {
        spdlog::critical("Failed to fd to epoll: " + util::errnoToString());
        exit(7);
    }
}

void SenderThreadManager::senderThreadFunction() {
    while(!mShouldQuit) {
        epoll_event events[20] = { 0 };
        int eventCount = epoll_wait(mEpollFd, events, 20, -1);
        if(eventCount < 0) {
            if(eventCount != EINTR) {
                spdlog::critical("epoll_wait(): {}", util::errnoToString());
            }
            // We got interrupted, which means there is data that we need to send.
            // Which socket we need to send data to is specified by mInitialSendTasks,
            // we handle that further down
        } else {
            for(int i = 0; i < eventCount; ++i) {
                if(events[i].events & EPOLLERR || events[i].events & EPOLLHUP) {
                    spdlog::error("Socket error!"); // TODO handle
                } else if(events[i].events & EPOLLOUT) {
                    // we can write some data!
                    auto[clientRef, clientRefLock] = mBridge.getClient(events[i].data.fd);
                    auto& client = clientRef.get();
                    auto[sendTasksRef, sendTasksRefLock] = client.getSendTasks();
                    auto& sendTasks = sendTasksRef.get();
                    if(sendTasks.empty()) {
                        continue;
                    }
                    // send data
                    for(auto& task: sendTasks) {
                        auto bytesSend = client.getTcpClient().send(task.data.data() + task.offset, task.data.size() - task.offset);
                        task.offset += bytesSend;
                    }
                }
            }
        }
        std::unique_lock<std::shared_mutex> lock{mInitialSendTasksMutex};
        for(auto& task: mInitialSendTasks) {
            auto bytesSend = task.client.get().getTcpClient().send(task.data.data(), task.data.size());
            if(bytesSend != task.data.size()) {
                auto[sendTasksRef, sendTasksRefLock] = task.client.get().getSendTasks();
                auto& sendTasks = sendTasksRef.get();
                sendTasks.emplace_back(MQTTClientConnection::SendTask{std::move(task.data), bytesSend});
            }
        }
        mInitialSendTasks.clear();
    }
}
void SenderThreadManager::sendData(MQTTClientConnection& client, std::vector<uint8_t>&& data) {
    std::unique_lock<std::shared_mutex> lock{mInitialSendTasksMutex};
    mInitialSendTasks.emplace_back(InitialSendTask{client, std::move(data)});
    lock.unlock();
    // pick a random thread to notify of the new task
    int tid = mSenderThreads.at(rand() % mSenderThreads.size()).native_handle();
    if(kill(tid, SIGUSR1) < 0) {
        spdlog::error("kill(): " + util::errnoToString());
    }

}

}
