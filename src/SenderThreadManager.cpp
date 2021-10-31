#include "SenderThreadManager.hpp"
#include <sys/epoll.h>
#include <csignal>


#include "MQTTClientConnection.hpp"

namespace nioev {

SenderThreadManager::SenderThreadManager(SenderThreadManagerExternalBridgeInterface& bridge, uint threadCount) : mBridge(bridge) {
    mEpollFd = epoll_create1(0);
    for(uint i = 0; i < threadCount; ++i) {
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

void SenderThreadManager::removeClientConnection(MQTTClientConnection& conn) {
    if(epoll_ctl(mEpollFd, EPOLL_CTL_DEL, conn.getTcpClient().getFd(), nullptr) < 0) {
        spdlog::critical("Failed to remove fd from epoll: {}", util::errnoToString());
    }
    /*std::lock_guard<std::shared_mutex> lock{mInitialSendTasksMutex};
    for(auto it = mInitialSendTasks.begin(); it != mInitialSendTasks.end();) {
        if(&it->client.get() == &conn) {
            it = mInitialSendTasks.erase(it);
        } else {
            it++;
        }
    }*/
}

void SenderThreadManager::senderThreadFunction() {
    while(!mShouldQuit) {
        epoll_event events[20] = { 0 };
        int eventCount = epoll_wait(mEpollFd, events, 20, -1);
        spdlog::debug("Sender waking up!");

        if(eventCount < 0) {
            if(errno != EINTR) {
                spdlog::critical("epoll_wait(): {}", util::errnoToString());
            }
            // We got interrupted, which means there is data that we need to send.
            // Which socket we need to send data to is specified by mInitialSendTasks,
            // we handle that further down
        } else {
            for(int i = 0; i < eventCount; ++i) {
                try {
                    if(events[i].events & EPOLLERR) {
                        throw std::runtime_error{"Socket error!"};
                    } else if(events[i].events & EPOLLOUT) {
                        // we can write some data!
                        auto [clientRef, clientRefLock] = mBridge.getClient(events[i].data.fd);
                        auto& client = clientRef.get();
                        auto [sendTasksRef, sendTasksRefLock] = client.getSendTasks();
                        auto& sendTasks = sendTasksRef.get();
                        if(sendTasks.empty()) {
                            continue;
                        }
                        // send data
                        for(auto& task : sendTasks) {
                            uint bytesSend = 0;
                            do {
                                bytesSend = client.getTcpClient().send(task.data.data() + task.offset, task.data.size() - task.offset);
                                task.offset += bytesSend;
                            } while(bytesSend > 0);
                        }
                    }
                } catch(std::exception& e) {
                    spdlog::error("Caught: {}", e.what());
                    mBridge.notifyConnectionError(events[i].data.fd);
                }
            }
        }
    }
}
void SenderThreadManager::sendPublish(MQTTClientConnection& conn, const std::string& topic, const std::vector<uint8_t>& msg, QoS qos) {
    util::BinaryEncoder encoder;
    uint8_t firstByte = static_cast<uint8_t>(QoS::QoS0) << 1; //FIXME use actual qos
    // TODO retain, dup
    firstByte |= static_cast<uint8_t>(MQTTMessageType::PUBLISH) << 4;
    encoder.encodeByte(firstByte);
    encoder.encodeString(topic);
    if(qos != QoS::QoS0) {
        // TODO add id
        assert(false);
    }
    encoder.encodeBytes(msg);
    encoder.insertPacketLength();
    sendData(conn, encoder.moveData());
}
void SenderThreadManager::sendData(MQTTClientConnection& conn, std::vector<uint8_t>&& bytes) {
    uint totalBytesSent = 0;
    uint bytesSent = 0;
    auto [_, sendLock] = conn.getSendTasks();
    do {
        bytesSent = conn.getTcpClient().send(bytes.data() + totalBytesSent, bytes.size() - totalBytesSent);
        totalBytesSent += bytesSent;
    } while(totalBytesSent < bytes.size());
    if(totalBytesSent < bytes.size()) {
        auto [sendTasksRef, sendTasksRefLock] = conn.getSendTasks();
        auto& sendTasks = sendTasksRef.get();
        sendTasks.emplace_back(MQTTClientConnection::SendTask{ std::move(bytes), totalBytesSent });
    }
}

}