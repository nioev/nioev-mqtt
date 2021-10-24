#include "ReceiverThreadManager.hpp"
#include <sys/epoll.h>
#include <spdlog/spdlog.h>
#include "Util.hpp"

#include "Enums.hpp"

namespace nioev {

ReceiverThreadManager::ReceiverThreadManager(ReceiverThreadManagerExternalBridgeInterface& bridge, uint threadCount)
: mBridge(bridge) {
    mEpollFd = epoll_create1(EPOLL_CLOEXEC);
    if(mEpollFd < 0) {
        spdlog::critical("Failed to create epoll fd: " + util::errnoToString());
        exit(5);
    }
    for(int i = 0; i < threadCount; ++i) {
        mReceiverThreads.emplace_back([this, i] {
            std::string threadName = "R-" + std::to_string(i);
            pthread_setname_np(pthread_self(), threadName.c_str());
            receiverThreadFunction();
        });
    }
}
void ReceiverThreadManager::receiverThreadFunction() {
    while(!mShouldQuit) {
        epoll_event events[20] = { 0 };
        int eventCount = epoll_wait(mEpollFd, events, 20, -1);
        if(eventCount < 0) {
            spdlog::critical("epoll_wait(): {}", util::errnoToString());
        }
        for(int i = 0; i < eventCount; ++i) {
            if(events[i].events & EPOLLERR || events[i].events & EPOLLHUP) {
                spdlog::info("Socket error!");
            } else if(events[i].events & EPOLLIN) {
                spdlog::info("Socket EPOLLIN!");
                auto& client = mBridge.getClient(events[i].data.fd);
                std::vector<uint8_t> bytes;
                // We receive bytes until there are none left (EAGAIN/EWOULDBLOCK). This is
                // the recommended way of doing io if one uses the edge-triggered mode of epoll.
                // This ensures that we don't hang somewhere when we couldn't receive all the data.
                do {
                    bytes = client.getTcpClient().recvAllAvailableBytes();
                    spdlog::info("Bytes read: {}", bytes.size());
                    // assert(bytes.size() > 0);
                    auto& recvData = client.getRecvData();

                    for(int i = 0; i < bytes.size(); ++i) {
                        switch(recvData.recvState) {
                        case MQTTClientConnection::PacketReceiveState::IDLE: {
                            recvData = {};
                            uint8_t packetTypeId = bytes.at(i) >> 4;
                            if(packetTypeId >= static_cast<int>(MQTTMessageType::Count) || packetTypeId == 0) {
                                spdlog::error("protocol violation"); // TODO
                            }
                            recvData.firstByte = bytes.at(i);
                            recvData.messageType = static_cast<MQTTMessageType>(packetTypeId);
                            recvData.currentReceiveBuffer.push_back(packetTypeId);
                            recvData.recvState = MQTTClientConnection::PacketReceiveState::RECEIVING_VAR_LENGTH;
                            break;
                        }
                        case MQTTClientConnection::PacketReceiveState::RECEIVING_VAR_LENGTH: {
                            uint8_t encodedByte = bytes.at(i);
                            recvData.packetLength += (encodedByte & 127) * recvData.multiplier;
                            recvData.multiplier *= 128;
                            if(recvData.multiplier > 128 * 128 * 128) {
                                throw std::runtime_error{ "Malformed remaining length" };
                            }
                            if((encodedByte & 128) == 0) {
                                recvData.recvState = MQTTClientConnection::PacketReceiveState::RECEIVING_DATA;
                            }
                            break;
                        }
                        case MQTTClientConnection::PacketReceiveState::RECEIVING_DATA:
                            if(bytes.size() <= recvData.packetLength) {
                                recvData.currentReceiveBuffer.insert(recvData.currentReceiveBuffer.end(), bytes.begin() + i, bytes.end());
                                i = bytes.size();
                            } else {
                                recvData.currentReceiveBuffer.insert(
                                    recvData.currentReceiveBuffer.end(), bytes.begin() + i, bytes.begin() + i + recvData.packetLength);
                                i += recvData.packetLength;
                            }
                            if(recvData.currentReceiveBuffer.size() >= recvData.packetLength) {
                                spdlog::info("Received packet of type {}", recvData.messageType);
                                recvData.recvState = MQTTClientConnection::PacketReceiveState::IDLE;
                            }
                            break;
                        }
                    }
                } while(!bytes.empty());
            }
        }
    }
}
void ReceiverThreadManager::addClientConnection(MQTTClientConnection& conn) {
    mClients.emplace_back(conn);
    epoll_event ev = { 0 };
    ev.data.fd = conn.getTcpClient().getFd();
    ev.events = EPOLLET | EPOLLIN | EPOLLEXCLUSIVE;
    if(epoll_ctl(mEpollFd, EPOLL_CTL_ADD, conn.getTcpClient().getFd(), &ev) < 0) {
        spdlog::critical("Failed to fd to epoll: " + util::errnoToString());
        exit(6);
    }
}

}
