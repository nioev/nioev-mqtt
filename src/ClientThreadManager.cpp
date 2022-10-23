#include "ClientThreadManager.hpp"
#include "nioev/lib/Util.hpp"
#include <spdlog/spdlog.h>
#include <sys/epoll.h>
#include <signal.h>
#include <unistd.h>

#include "nioev/lib/Enums.hpp"
#include "ApplicationState.hpp"

namespace nioev::mqtt {

void protocolViolation(const std::string& reason) {
    throw std::runtime_error{"Protocol violation: " + reason};
}

using namespace nioev::lib;

ClientThreadManager::ClientThreadManager(ApplicationState& app)
: mApp(app) {
    mEpollFd = epoll_create1(EPOLL_CLOEXEC);
    if(mEpollFd < 0) {
        spdlog::critical("Failed to create epoll fd: " + errnoToString());
        exit(5);
    }
    size_t threadCount = std::max<size_t>(4, std::thread::hardware_concurrency() / 2);
    for(size_t i = 0; i < threadCount; ++i) {
        mReceiverThreads.emplace_back([this, i] {
            std::string threadName = "C-" + std::to_string(i);
            pthread_setname_np(pthread_self(), threadName.c_str());
            sigset_t blockedSignals = { 0 };
            sigemptyset(&blockedSignals);
            sigaddset(&blockedSignals, SIGUSR1);
            pthread_sigmask(SIG_BLOCK, &blockedSignals, nullptr);
            receiverThreadFunction(i);
        });
    }
}
void ClientThreadManager::receiverThreadFunction(size_t threadId) {
    sigset_t blockedSignalsDuringEpoll = { 0 };
    sigemptyset(&blockedSignalsDuringEpoll);
    sigaddset(&blockedSignalsDuringEpoll, SIGINT);
    sigaddset(&blockedSignalsDuringEpoll, SIGTERM);
    std::vector<uint8_t> bytes;
    bytes.resize(64 * 1024 * 4);
    std::shared_lock<std::shared_mutex> suspendLock{mSuspendMutex};
    while(!mShouldQuit) {
        epoll_event events[128] = { 0 };
        int eventCount = epoll_pwait(mEpollFd, events, 128, -1, &blockedSignalsDuringEpoll);
        if(eventCount < 0) {
            if(errno == EINTR) {
                if(mShouldQuit)
                    continue;
                if(mShouldSuspend) {
                    suspendLock.unlock();
                    mSuspendMutex2.lock_shared();
                    mSuspendMutex2.unlock_shared();
                    suspendLock.lock();
                    continue;
                }
            } else {
                spdlog::warn("epoll_wait(): {}", errnoToString());
                continue;
            }
            // if an interrupt happens and we don't need to suspend, then we just continue onwards
        }
        if(threadId == 0) {
            if(!mRecentlyLoggedInClientsEmpty) {
                std::unique_lock<std::mutex> lock{mRecentlyLoggedInClientsMutex};
                while(!mRecentlyLoggedInClients.empty()) {
                    for(auto client: mRecentlyLoggedInClients) {
                        auto lock = client->getRecvMutexLock();
                        handlePacketsReceivedWhileConnecting(lock, *client);
                    }
                    mRecentlyLoggedInClients.clear();
                }
            }
        }
        for(int i = 0; i < eventCount; ++i) {
            auto& client = * (MQTTClientConnection*)events[i].data.ptr;
            try {
                if(events[i].events & EPOLLERR) {
                    client.getTcpClient().recv(bytes); // try to trigger proper error message
                    throw std::runtime_error{"Socket error!"};
                }
                if(events[i].events & EPOLLOUT) {
                    // we can write some data!

                    auto [sendTasksRef, sendTasksRefLock] = client.getSendTasks();
                    auto& sendTasks = sendTasksRef.get();
                    if(!sendTasks.empty()) {
                        client.getTcpClient().sendScatter(sendTasks.data(), sendTasks.size());
                        size_t donePackets = 0;
                        for(InTransitEncodedPacket& packet: sendTasks) {
                            if(packet.isDone()) {
                                donePackets += 1;
                            } else {
                                break;
                            }
                        }
                        sendTasks.erase(sendTasks.begin(), sendTasks.begin() + donePackets);
                        if(sendTasks.empty() && client.getStateAtomic() == MQTTClientConnection::ConnectionState::INVALID_PROTOCOL_VERSION) {
                            assert(sendTasks.empty());
                            throw CleanDisconnectException{};
                        }
                    }
                }
                if(events[i].events & (EPOLLIN | EPOLLHUP)) {
                    // we can read some data!
                    // We receive bytes until there are none left (EAGAIN/EWOULDBLOCK). This is
                    // the recommended way of doing io if one uses the edge-triggered mode of epoll.
                    // This ensures that we don't hang somewhere when we couldn't receive all the data.
                    uint bytesReceived = 0;
                    auto recvDataRefLock = client.getRecvMutexLock();

                    handlePacketsReceivedWhileConnecting(recvDataRefLock, client);

                    auto& recvData = client.getRecvData(recvDataRefLock);
                    do {
                        bytesReceived = client.getTcpClient().recv(bytes);
                        spdlog::debug("Bytes read: {}", bytesReceived);
                        if(bytesReceived > 0) {
                            client.setLastDataRecvTimestamp(std::chrono::steady_clock::now().time_since_epoch().count());
                        }
                        for(uint i = 0; i < bytesReceived;) {
                            switch(recvData.recvState) {
                            case MQTTClientConnection::PacketReceiveState::IDLE: {
                                recvData = {};
                                uint8_t packetTypeId = bytes.at(i) >> 4;
                                if(packetTypeId >= static_cast<int>(MQTTMessageType::Count) || packetTypeId == 0) {
                                    protocolViolation("Invalid message type");
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
                                    protocolViolation("Invalid message length");
                                }
                                if((encodedByte & 0x80) == 0) {
                                    if(recvData.packetLength == 0) {
                                        recvData.recvState = MQTTClientConnection::PacketReceiveState::IDLE;
                                        handlePacketReceived(mApp, client.getState(recvDataRefLock), client, recvData, recvDataRefLock);
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
                                    handlePacketReceived(mApp, client.getState(recvDataRefLock), client, recvData, recvDataRefLock);
                                    recvData.recvState = MQTTClientConnection::PacketReceiveState::IDLE;
                                }
                                break;
                            }
                            }
                        }
                    } while(bytesReceived > 0);
                }
            } catch(CleanDisconnectException&) {
                mApp.requestChange(ChangeRequestLogoutClient{&client});
            } catch(std::exception& e) {
                spdlog::error("Caught: {}", e.what());
                mApp.requestChange(ChangeRequestLogoutClient{&client});
            }
        }
    }
}
void ClientThreadManager::addClientConnection(MQTTClientConnection& conn) {
    epoll_event ev = { 0 };
    ev.data.fd = conn.getTcpClient().getFd();
    ev.data.ptr = &conn;
    ev.events = EPOLLET | EPOLLIN | EPOLLOUT | EPOLLEXCLUSIVE;
    if(epoll_ctl(mEpollFd, EPOLL_CTL_ADD, conn.getTcpClient().getFd(), &ev) < 0) {
        spdlog::critical("Failed to add fd to epoll: {}", lib::errnoToString());
        exit(6);
    }
}
void ClientThreadManager::removeClientConnection(MQTTClientConnection& conn) {
    if(epoll_ctl(mEpollFd, EPOLL_CTL_DEL, conn.getTcpClient().getFd(), nullptr) < 0) {
        spdlog::debug("Failed to remove fd from epoll: {}", lib::errnoToString());
    }
    std::unique_lock<std::mutex> lock{mRecentlyLoggedInClientsMutex};
    std::erase_if(mRecentlyLoggedInClients, [&](auto& rliClient) {
        return rliClient == &conn;
    });
}
void ClientThreadManager::addRecentlyLoggedInClient(MQTTClientConnection* client) {
    std::unique_lock<std::mutex> lock{mRecentlyLoggedInClientsMutex};
    mRecentlyLoggedInClients.emplace_back(client);
    lock.unlock();
    pthread_kill(mReceiverThreads.at(0).native_handle(), SIGUSR1);
}
void handlePacketReceived(ApplicationState& app, MQTTClientConnection::ConnectionState state, MQTTClientConnection& client, const MQTTClientConnection::PacketReceiveData& recvData, std::unique_lock<std::mutex>& clientReceiveLock) {
    spdlog::debug("Received packet of type {}", recvData.messageType);

    BinaryDecoder decoder{recvData.currentReceiveBuffer, recvData.packetLength};
    switch(state) {
    case MQTTClientConnection::ConnectionState::INITIAL: {
        switch(recvData.messageType) {
        case MQTTMessageType::CONNECT: {
            // initial connect
            constexpr uint8_t protocolName[] = { 0, 4, 'M', 'Q', 'T', 'T' };
            if(decoder.getCurrentRemainingLength() < 6) {
                protocolViolation("Missing MQTT protocol specifier");
            }
            if(memcmp(protocolName, decoder.getCurrentPtr(), 6) != 0) {
                protocolViolation("Invalid protocol version");
            }
            decoder.advance(6);

            uint8_t protocolLevel = decoder.decodeByte();
            if(protocolLevel != 4 && protocolLevel != 5) {
                // we only support MQTT 3.1.1
                BinaryEncoder response;
                response.encodeByte(2); // remaining packet length
                response.encodeByte(0); // no session present
                response.encodeByte(1); // invalid protocol version
                client.setStateAtomic(MQTTClientConnection::ConnectionState::INVALID_PROTOCOL_VERSION);
                spdlog::error("Invalid protocol version requested by client: {}", protocolLevel);
                client.sendData(EncodedPacket::fromData(static_cast<uint8_t>(MQTTMessageType::CONNACK) << 4, response.moveData()));
                break;
            }
            auto version = static_cast<MQTTVersion>(protocolLevel);
            client.setMQTTVersion(version);

            uint8_t connectFlags = decoder.decodeByte();
            uint16_t keepAlive = decoder.decode2Bytes();
            client.setKeepAliveIntervalSeconds(keepAlive);
            if(connectFlags & 0x1) {
                protocolViolation("Invalid connect flags bit set");
            }
            bool cleanSession = connectFlags & 0x2;
            if(version == MQTTVersion::V5) {
                client.setConnectProperties(decoder.decodeProperties());
            }

            auto clientId = decoder.decodeString();
            if(clientId.empty() && !cleanSession) {
                protocolViolation("Invalid client id");
            }
            if(connectFlags & 0x4) {
                // will message exists
                auto willTopic = decoder.decodeString();
                if(willTopic.empty()) {
                    protocolViolation("Invalid will topic");
                }
                auto willMessage = decoder.decodeBytesWithPrefixLength();
                auto willQos = (connectFlags & 0x18) >> 3;
                auto willRetain = static_cast<Retain>(!!(connectFlags & 0x20));
                if(willQos >= 3) {
                    protocolViolation("Invalid will qos");
                }
                client.setWill(clientReceiveLock, std::move(willTopic), std::move(willMessage), static_cast<QoS>(willQos), willRetain);
            }
            if(connectFlags & 0x80) {
                // username
                auto username = decoder.decodeString();
            }
            if(connectFlags & 0x40) {
                // password
                auto password = decoder.decodeString();
            }
            client.setStateAtomic(MQTTClientConnection::ConnectionState::CONNECTING);
            app.requestChange(ChangeRequestLoginClient{&client, std::move(clientId), cleanSession ? CleanSession::Yes : CleanSession::No});

            break;
        }
        default: {
            protocolViolation("Packet not allowed here");
        }
        }
        break;
    }
    case MQTTClientConnection::ConnectionState::CONNECTING: {
        client.pushPacketReceivedWhileConnecting(clientReceiveLock, recvData);
        break;
    }
    case MQTTClientConnection::ConnectionState::CONNECTED: {
        switch(recvData.messageType) {
        case MQTTMessageType::PUBLISH: {
            bool dup = recvData.firstByte & 0x8; // TODO handle
            uint8_t qosInt = (recvData.firstByte >> 1) & 0x3;
            if(qosInt >= 3) {
                protocolViolation("Invalid QoS");
            }
            QoS qos = static_cast<QoS>(qosInt);
            auto retain = static_cast<Retain>(!!(recvData.firstByte & 0x1));
            auto topic = decoder.decodeString(); // TODO check for allowed chars
            if(topic.empty()) {
                protocolViolation("Invalid topic");
            }
            bool doDeliverOnward = true;
            if(qos == QoS::QoS1 || qos == QoS::QoS2) {
                auto id = decoder.decode2Bytes();
                if(qos == QoS::QoS1) {
                    // send PUBACK
                    BinaryEncoder encoder;
                    encoder.encode2Bytes(id);

                    if(client.getMQTTVersion() == MQTTVersion::V5) {
                        PropertyList properties;
                        encoder.encodeByte(0); // Reason code success
                        encoder.encodePropertyList(properties);
                    }
                    client.sendData(EncodedPacket::fromData(static_cast<uint8_t>(MQTTMessageType::PUBACK) << 4, encoder.moveData()));
                } else {
                    auto state = client.getPersistentClientState();
                    if(!state)
                        throw std::runtime_error{"Persistent state lost!"};
                    auto lock = state->getLock();
                    auto& recvPacketIds = state->getQoS2ReceivingPacketIds();
                    doDeliverOnward = !recvPacketIds[id];
                    recvPacketIds[id] = true;
                    lock.unlock();
                    // send PUBREC
                    BinaryEncoder encoder;
                    encoder.encode2Bytes(id);
                    if(client.getMQTTVersion() == MQTTVersion::V5) {
                        PropertyList properties;
                        encoder.encodeByte(0); // Reason code success
                        encoder.encodePropertyList(properties);
                    }
                    client.sendData(EncodedPacket::fromData(static_cast<uint8_t>(MQTTMessageType::PUBREC) << 4, encoder.moveData()));
                }
            }
            if(doDeliverOnward) {
                PropertyList properties;
                if(client.getMQTTVersion() == MQTTVersion::V5) {
                    properties = decoder.decodeProperties();
                }
                app.publish(std::move(topic), decoder.getRemainingBytes(), qos, retain, properties);
            }
            break;
        }
        case MQTTMessageType::PUBACK: {
            uint16_t id = decoder.decode2Bytes();
            PropertyList properties;
            if(client.getMQTTVersion() == MQTTVersion::V5) {
                uint8_t reasonCode = decoder.decodeByte();
                properties = decoder.decodeProperties();
            }
            auto state = client.getPersistentClientState();
            if(!state)
                throw std::runtime_error{"Persistent state lost!"};
            auto lock = state->getLock();
            auto& sendingPackets = state->getHighQoSSendingPackets();
            sendingPackets.erase(id);
            break;
        }
        case MQTTMessageType::PUBREL: {
            // QoS 2 part 2 (receiving packet)
            if(recvData.firstByte != 0x62) {
                protocolViolation("Invalid first byte in PUBREL");
            }
            uint16_t id = decoder.decode2Bytes();
            PropertyList properties;
            if(client.getMQTTVersion() == MQTTVersion::V5) {
                uint8_t reasonCode = decoder.decodeByte();
                properties = decoder.decodeProperties();
            }
            bool packetIdentifierFound = true;
            {
                auto state = client.getPersistentClientState();
                if(!state)
                    throw std::runtime_error{"Persistent state lost!"};
                auto stateLock = state->getLock();
                auto &receivingPacketIds = state->getQoS2ReceivingPacketIds();
                if(!receivingPacketIds[id]) {
                    stateLock.unlock();
                    packetIdentifierFound = false;
                    spdlog::warn("[{}] PUBREL no such message id", client.getClientId());
                } else {
                    receivingPacketIds[id] = false;
                }
            }

            // send PUBCOMP
            BinaryEncoder encoder;
            encoder.encode2Bytes(id);
            if(client.getMQTTVersion() == MQTTVersion::V5) {
                if(!packetIdentifierFound) {
                    PropertyList properties;
                    encoder.encodeByte(0x92); // Reason code identifier nout found
                    encoder.encodePropertyList(properties);
                }
            }
            client.sendData(EncodedPacket::fromData(static_cast<uint8_t>(MQTTMessageType::PUBCOMP) << 4, encoder.moveData()));
            break;
        }
        case MQTTMessageType::PUBREC: {
            // QoS 2 part 1 (sending packets)
            uint16_t id = decoder.decode2Bytes();
            PropertyList properties;
            if(client.getMQTTVersion() == MQTTVersion::V5) {
                uint8_t reasonCode = decoder.decodeByte();
                properties = decoder.decodeProperties();
            }
            bool packetIdentifierFound = true;
            {
                auto state = client.getPersistentClientState();
                if(!state)
                    throw std::runtime_error{"Persistent state lost!"};
                auto stateLock = state->getLock();
                auto &sendingPackets = state->getHighQoSSendingPackets();
                if(sendingPackets.erase(id) != 1) {
                    stateLock.unlock();
                    packetIdentifierFound = false;
                    spdlog::warn("[{}] PUBREC no such message id", client.getClientId());
                }
                state->getQoS2PubRecReceived()[id] = true;
            }

            // send PUBREL
            BinaryEncoder encoder;
            encoder.encode2Bytes(id);
            if(client.getMQTTVersion() == MQTTVersion::V5) {
                if(!packetIdentifierFound) {
                    PropertyList properties;
                    encoder.encodeByte(0x92);
                    encoder.encodePropertyList(properties);
                }
            }
            client.sendData(EncodedPacket::fromData((static_cast<uint8_t>(MQTTMessageType::PUBREL) << 4) | 0b10, encoder.moveData()));
            break;
        }
        case MQTTMessageType::PUBCOMP: {
            // QoS 2 part 3 (sending packets)
            uint16_t id = decoder.decode2Bytes();
            PropertyList properties;
            if(client.getMQTTVersion() == MQTTVersion::V5) {
                uint8_t reasonCode = decoder.decodeByte();
                properties = decoder.decodeProperties();
            }
            auto state = client.getPersistentClientState();
            if(!state)
                throw std::runtime_error{"Persistent state lost!"};

            state->getQoS2PubRecReceived()[id] = false;
            break;
        }
        case MQTTMessageType::SUBSCRIBE: {
            if(recvData.firstByte != 0x82) {
                protocolViolation("SUBSCRIBE invalid first byte");
            }
            auto packetIdentifier = decoder.decode2Bytes();
            PropertyList properties;
            if(client.getMQTTVersion() == MQTTVersion::V5) {
                properties = decoder.decodeProperties();
            }

            BinaryEncoder encoder;
            encoder.encode2Bytes(packetIdentifier);

            if(client.getMQTTVersion() == MQTTVersion::V5) {
                PropertyList properties;
                encoder.encodePropertyList(properties);
            }

            do {
                auto topic = decoder.decodeString();
                if(topic.empty()) {
                    protocolViolation("SUBSCRIBE topic is empty");
                }
                spdlog::info("[{}] Subscribing to {}", client.getClientId(), topic);
                uint8_t qosInt = decoder.decodeByte();
                if(qosInt >= 3) {
                    protocolViolation("SUBSCRIPE invalid qos");
                }
                auto qos = static_cast<QoS>(qosInt);
                encoder.encodeByte(qosInt);
                auto state = client.getPersistentClientState();
                if(!state)
                    throw std::runtime_error{"Persistent state lost!"};
                app.requestChange(ChangeRequestSubscribe{state, std::move(topic), qos});
            } while(!decoder.empty());

            client.sendData(EncodedPacket::fromData(static_cast<uint8_t>(MQTTMessageType::SUBACK) << 4, encoder.moveData()));
            break;
        }
        case MQTTMessageType::UNSUBSCRIBE: {
            if(recvData.firstByte != 0xA2) {
                protocolViolation("UNSUBSCRIBE invalid first byte");
            }
            auto packetIdentifier = decoder.decode2Bytes();
            PropertyList properties;
            if(client.getMQTTVersion() == MQTTVersion::V5) {
                properties = decoder.decodeProperties();
            }

            // prepare SUBACK
            BinaryEncoder encoder;
            encoder.encode2Bytes(packetIdentifier);
            if(client.getMQTTVersion() == MQTTVersion::V5) {
                PropertyList properties;
                encoder.encodePropertyList(properties);
            }

            do {
                auto topic = decoder.decodeString();
                auto state = client.getPersistentClientState();
                if(!state)
                    throw std::runtime_error{"Persistent state lost!"};
                app.requestChange(ChangeRequestUnsubscribe{state, topic});
                if(client.getMQTTVersion() == MQTTVersion::V5) {
                    encoder.encodeByte(0); // success - according to spec we should actually send 0x11 if the sub didn't already exist
                }
            } while(!decoder.empty());

            client.sendData(EncodedPacket::fromData(static_cast<uint8_t>(MQTTMessageType::UNSUBACK) << 4, encoder.moveData()));

            break;
        }
        case MQTTMessageType::PINGREQ: {
            if(recvData.firstByte != 0xC0) {
                protocolViolation("PINGREQ Invalid first byte");
            }
            BinaryEncoder encoder;
            client.sendData(EncodedPacket::fromFirstByte(static_cast<uint8_t>(MQTTMessageType::PINGRESP) << 4));
            spdlog::debug("Replying to PINGREQ");
            break;
        }
        case MQTTMessageType::DISCONNECT: {
            if(recvData.firstByte != 0xE0) {
                protocolViolation("Invalid disconnect first byte");
            }
            PropertyList properties;
            if(client.getMQTTVersion() == MQTTVersion::V5 && decoder.getCurrentRemainingLength() > 1) {
                properties = decoder.decodeProperties();
            }
            spdlog::info("[{}] Received disconnect request", client.getClientId());
            client.discardWill(clientReceiveLock);
            throw CleanDisconnectException{};

            break;
        }
        }
        break;
    }
    }
}

ClientThreadManager::~ClientThreadManager() {
    mShouldQuit = true;
    for(auto& t : mReceiverThreads) {
        pthread_kill(t.native_handle(), SIGUSR1);
        t.join();
    }
    if(close(mEpollFd)) {
        spdlog::error("close(): {}", lib::errnoToString());
    }
}
void ClientThreadManager::suspendAllThreads() {
    mShouldSuspend = true;
    mSuspendMutex2.lock();
    while(!mSuspendMutex.try_lock()) {
        for(auto& thread: mReceiverThreads) {
            pthread_kill(thread.native_handle(), SIGUSR1);
        }
        std::this_thread::yield();
    }
    mSuspendMutex2.unlock();
}
void ClientThreadManager::resumeAllThreads() {
    mSuspendMutex.unlock();
    mShouldSuspend = false;
}
void ClientThreadManager::handlePacketsReceivedWhileConnecting(std::unique_lock<std::mutex>& recvDataLock, MQTTClientConnection& client) {
    for(auto& packet: client.getPacketsReceivedWhileConnecting(recvDataLock)) {
        handlePacketReceived(mApp, MQTTClientConnection::ConnectionState::CONNECTED, client, packet, recvDataLock);
    }
    client.getPacketsReceivedWhileConnecting(recvDataLock).clear();
}

}
