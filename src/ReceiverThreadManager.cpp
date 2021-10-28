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
    for(uint i = 0; i < threadCount; ++i) {
        mReceiverThreads.emplace_back([this, i] {
            std::string threadName = "R-" + std::to_string(i);
            pthread_setname_np(pthread_self(), threadName.c_str());
            receiverThreadFunction();
        });
    }
}
void ReceiverThreadManager::receiverThreadFunction() {
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
                } else if(events[i].events & EPOLLIN) {
                    spdlog::debug("Socket EPOLLIN!");
                    auto [clientRef, clientRefLock] = mBridge.getClient(events[i].data.fd);
                    auto& client = clientRef.get();
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
                                if((encodedByte & 128) == 0) {
                                    recvData.recvState = MQTTClientConnection::PacketReceiveState::RECEIVING_DATA;
                                    spdlog::debug("Expecting packet of length {}", recvData.packetLength);
                                }
                                i += 1;
                                break;
                            }
                            case MQTTClientConnection::PacketReceiveState::RECEIVING_DATA:
                                if(bytesReceived - i <= recvData.packetLength) {
                                    recvData.currentReceiveBuffer.insert(recvData.currentReceiveBuffer.end(), bytes.begin() + i, bytes.end());
                                    i = bytesReceived;
                                } else {
                                    recvData.currentReceiveBuffer.insert(
                                        recvData.currentReceiveBuffer.end(), bytes.begin() + i, bytes.begin() + i + recvData.packetLength);
                                    i += recvData.packetLength;
                                }
                                if(recvData.currentReceiveBuffer.size() >= recvData.packetLength) {
                                    spdlog::debug("Received: {}", recvData.currentReceiveBuffer.size());
                                    handlePacketReceived(client, recvData);
                                    recvData.recvState = MQTTClientConnection::PacketReceiveState::IDLE;
                                }
                                break;
                            }
                        }
                    } while(bytesReceived > 0);
                }
            } catch(std::exception& e) {
                spdlog::error("Caught: {}", e.what());
                mBridge.notifyConnectionError(events[i].data.fd);
            }
        }
    }
}
void ReceiverThreadManager::addClientConnection(MQTTClientConnection& conn) {
    epoll_event ev = { 0 };
    ev.data.fd = conn.getTcpClient().getFd();
    ev.events = EPOLLET | EPOLLIN | EPOLLEXCLUSIVE;
    // TODO save pointer to client
    if(epoll_ctl(mEpollFd, EPOLL_CTL_ADD, conn.getTcpClient().getFd(), &ev) < 0) {
        spdlog::critical("Failed to add fd to epoll: {}", util::errnoToString());
        exit(6);
    }
}
void ReceiverThreadManager::removeClientConnection(MQTTClientConnection& conn) {
    if(epoll_ctl(mEpollFd, EPOLL_CTL_DEL, conn.getTcpClient().getFd(), nullptr) < 0) {
        spdlog::critical("Failed to remove fd from epoll: {}", util::errnoToString());
    }
}
void ReceiverThreadManager::handlePacketReceived(MQTTClientConnection& client, const MQTTClientConnection::PacketReceiveData& recvData) {
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
                mBridge.sendData(client, std::move(response));
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
                if(willQos >= 3) {
                    protocolViolation();
                }
                client.setWill(std::move(willTopic), std::move(willMessage), static_cast<QoS>(willQos));
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


            mBridge.sendData(client, std::move(response));
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
            bool retain = recvData.firstByte & 0x1;
            auto topic = decoder.decodeString(); // TODO check for allowed chars
            if(qos == QoS::QoS1 || qos == QoS::QoS2) {
                auto id = decoder.decode2Bytes(); // TODO use
            }
            std::vector<uint8_t> data = decoder.getRemainingBytes();
            mBridge.publish(topic, data, qos);
            break;
        }
        case MQTTMessageType::SUBSCRIBE: {
            if(recvData.firstByte != 0x82) {
                protocolViolation();
            }
            auto packetIdentifier = decoder.decode2Bytes();
            do {
                auto topic = decoder.decodeString();
                uint8_t qosInt = decoder.decodeByte();
                if(qosInt >= 3) {
                    protocolViolation();
                }
                auto qos = static_cast<QoS>(qosInt);
                mBridge.addSubscription(client, std::move(topic), qos);
            } while(!decoder.empty());

            // prepare SUBACK
            util::BinaryEncoder encoder;
            encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::SUBACK) << 4);
            encoder.encode2Bytes(packetIdentifier);
            encoder.encodeByte(0); // maximum QoS 0 TODO support more
            encoder.insertPacketLength();
            mBridge.sendData(client, encoder.moveData());

            break;
        }
        }
        break;
    }
    }
}
void ReceiverThreadManager::protocolViolation() {
    throw std::runtime_error{"Protocol violation"};
}

}
