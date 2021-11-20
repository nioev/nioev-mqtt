#include "ClientThreadManager.hpp"
#include "Util.hpp"
#include <spdlog/spdlog.h>
#include <sys/epoll.h>
#include <signal.h>

#include "Enums.hpp"
#include "Application.hpp"
#include "Util.hpp"

namespace nioev {

ClientThreadManager::ClientThreadManager(Application& app, uint threadCount)
: mApp(app) {
    mEpollFd = epoll_create1(EPOLL_CLOEXEC);
    if(mEpollFd < 0) {
        spdlog::critical("Failed to create epoll fd: " + util::errnoToString());
        exit(5);
    }
    for(uint i = 0; i < threadCount; ++i) {
        mReceiverThreads.emplace_back([this, i] {
            std::string threadName = "C-" + std::to_string(i);
            pthread_setname_np(pthread_self(), threadName.c_str());
            receiverThreadFunction();
        });
    }
}
void ClientThreadManager::receiverThreadFunction() {
    std::vector<uint8_t> bytes;
    bytes.resize(64 * 1024);
    while(!mShouldQuit) {
        epoll_event events[20] = { 0 };
        int eventCount = epoll_wait(mEpollFd, events, 20, -1);
        if(eventCount < 0) {
            spdlog::critical("epoll_wait(): {}", util::errnoToString());
        }
        for(int i = 0; i < eventCount; ++i) {
            try {
                if(events[i].events & EPOLLERR) {
                    throw std::runtime_error{"Socket error!"};
                }
                auto [clientRef, clientRefLock] = mApp.getClient(events[i].data.fd);
                auto& client = clientRef.get();
                if(events[i].events & EPOLLOUT) {
                    // we can write some data!
                    auto [sendTasksRef, sendTasksRefLock] = client.getSendTasks();
                    auto& sendTasks = sendTasksRef.get();
                    if(sendTasks.empty()) {
                        continue;
                    }
                    // send data
                    for(auto it = sendTasks.begin(); it != sendTasks.end();) {
                        uint bytesSend = 0;
                        do {
                            bytesSend = client.getTcpClient().send(it->data.data() + it->offset, it->data.size() - it->offset);
                            it->offset += bytesSend;
                        } while(bytesSend > 0);

                        if(it->offset >= it->data.size()) {
                            if(sendTasks.size() == 1) {
                                epoll_event ev = { 0 };
                                ev.data.fd = client.getTcpClient().getFd();
                                ev.events = EPOLLET | EPOLLIN | EPOLLEXCLUSIVE;
                                if(epoll_ctl(mEpollFd, EPOLL_CTL_MOD, client.getTcpClient().getFd(), &ev) < 0) {
                                    spdlog::warn("epoll_ctl(): {}", util::errnoToString());
                                    throw std::runtime_error{"epoll_ctl(): " + util::errnoToString()};
                                }
                            }
                            it = sendTasks.erase(it);
                        } else {
                            break;
                        }
                    }
                }
                if(events[i].events & EPOLLIN) {
                    // we can read some data!
                    // We receive bytes until there are none left (EAGAIN/EWOULDBLOCK). This is
                    // the recommended way of doing io if one uses the edge-triggered mode of epoll.
                    // This ensures that we don't hang somewhere when we couldn't receive all the data.
                    uint bytesReceived = 0;
                    auto [recvDataRef, recvDataRefLock] = client.getRecvData();
                    do {
                        bytesReceived = client.getTcpClient().recv(bytes);
                        spdlog::debug("Bytes read: {}", bytesReceived);
                        auto& recvData = recvDataRef.get();
                        for(uint i = 0; i < bytesReceived;) {
                            switch(recvData.recvState) {
                            case MQTTClientConnection::PacketReceiveState::IDLE: {
                                recvData = {};
                                uint8_t packetTypeId = bytes.at(i) >> 4;
                                if(packetTypeId >= static_cast<int>(MQTTMessageType::Count) || packetTypeId == 0) {
                                    protocolViolation();
                                }
                                recvData.firstByte = bytes.at(i);
                                recvData.messageType = static_cast<MQTTMessageType>(packetTypeId);
                                recvData.recvState = MQTTClientConnection::PacketReceiveState::RECEIVING_VAR_LENGTH;
                                i += 1;
                                break;
                            }
                            case MQTTClientConnection::PacketReceiveState::RECEIVING_VAR_LENGTH: {
                                uint8_t encodedByte = bytes.at(i);
                                recvData.packetLength += (encodedByte & 127) * recvData.multiplier;
                                recvData.multiplier *= 128;
                                if(recvData.multiplier > 128 * 128 * 128) {
                                    protocolViolation();
                                }
                                if((encodedByte & 0x80) == 0) {
                                    if(recvData.packetLength == 0) {
                                        recvData.recvState = MQTTClientConnection::PacketReceiveState::IDLE;
                                        handlePacketReceived(client, recvData);
                                    } else {
                                       recvData.recvState = MQTTClientConnection::PacketReceiveState::RECEIVING_DATA;
                                       spdlog::debug("Expecting packet of length {}", recvData.packetLength);
                                    }
                                }
                                i += 1;
                                break;
                            }
                            case MQTTClientConnection::PacketReceiveState::RECEIVING_DATA: {
                                uint remainingReceivedSize = bytesReceived - i;
                                assert(recvData.currentReceiveBuffer.size() <= recvData.packetLength);
                                if(remainingReceivedSize <= recvData.packetLength - recvData.currentReceiveBuffer.size()) {
                                    recvData.currentReceiveBuffer.insert(
                                        recvData.currentReceiveBuffer.end(), bytes.begin() + i, bytes.begin() + i + remainingReceivedSize);
                                    i = bytesReceived; // break
                                } else {
                                    const uint bytesToCopy = recvData.packetLength - recvData.currentReceiveBuffer.size();
                                    recvData.currentReceiveBuffer.insert(
                                        recvData.currentReceiveBuffer.end(), bytes.begin() + i, bytes.begin() + i + bytesToCopy);
                                    i += bytesToCopy;
                                }
                                if(recvData.currentReceiveBuffer.size() == recvData.packetLength) {
                                    spdlog::debug("Received: {}", recvData.currentReceiveBuffer.size());
                                    handlePacketReceived(client, recvData);
                                    recvData.recvState = MQTTClientConnection::PacketReceiveState::IDLE;
                                }
                                break;
                            }
                            }
                        }
                    } while(bytesReceived > 0);
                }
            } catch(CleanDisconnectException&) {
                try {
                    auto[client, lock] = mApp.getClient(events[i].data.fd);
                    client.get().notifyConnecionError();
                } catch(...) {
                    // ignore errors while fetching the client, it probably just means it has been disconnected already
                }
            } catch(std::exception& e) {
                spdlog::error("Caught: {}", e.what());
                try {
                    auto[client, lock] = mApp.getClient(events[i].data.fd);
                    client.get().notifyConnecionError();
                } catch(...) {
                    // ignore errors while fetching the client, it probably just means it has been disconnected already
                }
            }
        }
    }
}
void ClientThreadManager::addClientConnection(MQTTClientConnection& conn) {
    epoll_event ev = { 0 };
    ev.data.fd = conn.getTcpClient().getFd();
    ev.events = EPOLLET | EPOLLIN | EPOLLEXCLUSIVE;
    // TODO save pointer to client
    if(epoll_ctl(mEpollFd, EPOLL_CTL_ADD, conn.getTcpClient().getFd(), &ev) < 0) {
        spdlog::critical("Failed to add fd to epoll: {}", util::errnoToString());
        exit(6);
    }
}
void ClientThreadManager::removeClientConnection(MQTTClientConnection& conn) {
    if(epoll_ctl(mEpollFd, EPOLL_CTL_DEL, conn.getTcpClient().getFd(), nullptr) < 0) {
        spdlog::critical("Failed to remove fd from epoll: {}", util::errnoToString());
    }
}
void ClientThreadManager::handlePacketReceived(MQTTClientConnection& client, const MQTTClientConnection::PacketReceiveData& recvData) {
    spdlog::debug("Received packet of type {}", recvData.messageType);

    util::BinaryDecoder decoder{recvData.currentReceiveBuffer, recvData.packetLength};
    switch(client.getState()) {
    case MQTTClientConnection::ConnectionState::INITIAL: {
        switch(recvData.messageType) {
        case MQTTMessageType::CONNECT: {
            // initial connect
            constexpr uint8_t protocolName[] = { 0, 4, 'M', 'Q', 'T', 'T' };
            if(memcmp(protocolName, decoder.getCurrentPtr(), 6) != 0) {
                protocolViolation();
            }
            decoder.advance(6);
            std::vector<uint8_t> response;
            response.push_back(static_cast<uint8_t>(MQTTMessageType::CONNACK) << 4);
            response.push_back(2); // remaining packet length

            uint8_t protocolLevel = decoder.decodeByte();
            if(protocolLevel != 4) {
                // we only support MQTT 3.1.1
                response.push_back(0); // no session present
                response.push_back(1); // invalid protocol version
                client.setState(MQTTClientConnection::ConnectionState::INVALID_PROTOCOL_VERSION);
                spdlog::error("Invalid protocol version requested by client: {}", protocolLevel);
                sendData(client, std::move(response));
                break;
            }
            uint8_t connectFlags = decoder.decodeByte();
            uint16_t keepAlive = decoder.decode2Bytes();
            if(connectFlags & 0x1) {
                protocolViolation();
            }
            bool cleanSession = connectFlags & 0x2;
            auto clientId = decoder.decodeString();
            if(clientId.empty() && !cleanSession) {
                protocolViolation();
            }
            if(connectFlags & 0x4) {
                // will message exists
                auto willTopic = decoder.decodeString();
                auto willMessage = decoder.decodeBytesWithPrefixLength();
                auto willQos = (connectFlags & 0x18) >> 3;
                auto willRetain = static_cast<Retain>(!!(connectFlags & 0x20));
                if(willQos >= 3) {
                    protocolViolation();
                }
                client.setWill(std::move(willTopic), std::move(willMessage), static_cast<QoS>(willQos), willRetain);
            }
            if(connectFlags & 0x80) {
                // username
                auto username = decoder.decodeString();
            }
            if(connectFlags & 0x40) {
                // password
                auto password = decoder.decodeString();
            }
            response.push_back(0); // no session present TODO fix
            response.push_back(0); // everything okay
            spdlog::debug("Sent response!");


            sendData(client, std::move(response));
            client.setState(MQTTClientConnection::ConnectionState::CONNECTED);
            break;
        }
        default: {
            protocolViolation();
        }
        }
        break;
    }
    case MQTTClientConnection::ConnectionState::CONNECTED: {
        switch(recvData.messageType) {
        case MQTTMessageType::PUBLISH: {
            bool dup = recvData.firstByte & 0x8; // TODO handle
            uint8_t qosInt = (recvData.firstByte >> 1) & 0x3;
            if(qosInt >= 3) {
                protocolViolation();
            }
            QoS qos = static_cast<QoS>(qosInt);
            auto retain = static_cast<Retain>(!!(recvData.firstByte & 0x1));
            auto topic = decoder.decodeString(); // TODO check for allowed chars
            if(qos == QoS::QoS1 || qos == QoS::QoS2) {
                auto id = decoder.decode2Bytes(); // TODO use
                if(qos == QoS::QoS1) {
                    // send PUBACK
                    util::BinaryEncoder encoder;
                    encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::PUBACK) << 4);
                    encoder.encode2Bytes(id);
                    encoder.insertPacketLength();
                    sendData(client, encoder.moveData());
                }
            }
            std::vector<uint8_t> data = decoder.getRemainingBytes();
            // we need to go through the app to 1. save retained messages 2. interact with scripts
            mApp.publish(std::move(topic), std::move(data), qos, retain);
            break;
        }
        case MQTTMessageType::SUBSCRIBE: {
            if(recvData.firstByte != 0x82) {
                protocolViolation();
            }
            auto packetIdentifier = decoder.decode2Bytes();
            do {
                auto topic = decoder.decodeString();
                spdlog::info("[{}:{}] Subscribing to {}", client.getTcpClient().getRemoteIp(), client.getTcpClient().getRemotePort(), topic);
                uint8_t qosInt = decoder.decodeByte();
                if(qosInt >= 3) {
                    protocolViolation();
                }
                auto qos = static_cast<QoS>(qosInt);
                mApp.addSubscription(client, std::move(topic), qos);
            } while(!decoder.empty());


            // prepare SUBACK
            util::BinaryEncoder encoder;
            encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::SUBACK) << 4);
            encoder.encode2Bytes(packetIdentifier);
            encoder.encodeByte(0); // maximum QoS 0 TODO support more
            encoder.insertPacketLength();
            sendData(client, encoder.moveData());

            break;
        }
        case MQTTMessageType::UNSUBSCRIBE: {
            if(recvData.firstByte != 0xA2) {
                protocolViolation();
            }
            auto packetIdentifier = decoder.decode2Bytes();
            do {
                auto topic = decoder.decodeString();
                mApp.deleteSubscription(client, topic);
            } while(!decoder.empty());

            // prepare SUBACK
            util::BinaryEncoder encoder;
            encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::UNSUBACK) << 4);
            encoder.encode2Bytes(packetIdentifier);
            encoder.insertPacketLength();
            sendData(client, encoder.moveData());

            break;
        }
        case MQTTMessageType::PINGREQ: {
            if(recvData.firstByte != 0xC0) {
                protocolViolation();
            }
            util::BinaryEncoder encoder;
            encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::PINGRESP) << 4);
            encoder.insertPacketLength();
            sendData(client, encoder.moveData());
            spdlog::debug("Replying to PINGREQ");
            break;
        }
        case MQTTMessageType::DISCONNECT: {
            if(recvData.firstByte != 0xE0) {
                protocolViolation();
            }
            spdlog::info("[{}:{}] Disconnecting...", client.getTcpClient().getRemoteIp(), client.getTcpClient().getRemotePort());
            client.discardWill();
            throw CleanDisconnectException{};

            break;
        }
        }
        break;
    }
    }
}
void ClientThreadManager::protocolViolation() {
    throw std::runtime_error{"Protocol violation"};
}

void ClientThreadManager::sendPublish(MQTTClientConnection& conn, const std::string& topic, const std::vector<uint8_t>& msg, QoS qos, Retained retained) {
    util::BinaryEncoder encoder;
    uint8_t firstByte = static_cast<uint8_t>(QoS::QoS0) << 1; //FIXME use actual qos
    // TODO retain, dup
    if(retained == Retained::Yes) {
        firstByte |= 1;
    }
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
void ClientThreadManager::sendData(MQTTClientConnection& conn, std::vector<uint8_t>&& bytes) {
    try {
        uint totalBytesSent = 0;
        uint bytesSent = 0;
        auto [sendTasksRef, sendLock] = conn.getSendTasks();
        auto& sendTasks = sendTasksRef.get();
        do {
            bytesSent = conn.getTcpClient().send(bytes.data() + totalBytesSent, bytes.size() - totalBytesSent);
            totalBytesSent += bytesSent;
        } while(bytesSent > 0 && totalBytesSent < bytes.size());
        //spdlog::warn("Bytes sent: {}, Total bytes sent: {}", bytesSent, totalBytesSent);

        if(totalBytesSent < bytes.size()) {
            sendTasks.emplace_back(MQTTClientConnection::SendTask{ std::move(bytes), totalBytesSent });

            // listen for EPOLLOUT
            // if we specify EPOLLEXCLUSIVE, we need to delete and readd the FD
            if(epoll_ctl(mEpollFd, EPOLL_CTL_DEL, conn.getTcpClient().getFd(), nullptr) < 0) {
                spdlog::warn("epoll_ctl(EPOLL_CTL_DEL): {}", util::errnoToString());
                throw std::runtime_error{"epoll_ctl(EPOLL_CTL_DEL): " + util::errnoToString()};
            }
            epoll_event ev = { 0 };
            ev.data.fd = conn.getTcpClient().getFd();
            ev.events = EPOLLET | EPOLLIN | EPOLLOUT | EPOLLEXCLUSIVE;
            // TODO save pointer to client
            if(epoll_ctl(mEpollFd, EPOLL_CTL_ADD, conn.getTcpClient().getFd(), &ev) < 0) {
                spdlog::warn("epoll_ctl(EPOLL_CTL_ADD): {}", util::errnoToString());
                throw std::runtime_error{"epoll_ctl(EPOLL_CTL_ADD): " + util::errnoToString()};
            }
        }
    } catch(std::exception& e) {
        spdlog::error("Error while sending data: {}", e.what());
        conn.notifyConnecionError();
    }
}
ClientThreadManager::~ClientThreadManager() {
    mShouldQuit = true;
    for(auto& t : mReceiverThreads) {
        pthread_kill(t.native_handle(), SIGUSR1);
        t.join();
    }
    if(close(mEpollFd)) {
        spdlog::error("close(): {}", util::errnoToString());
    }
}

}
