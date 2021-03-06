#include "ClientThreadManager.hpp"
#include "nioev/lib/Util.hpp"
#include <spdlog/spdlog.h>
#include <sys/epoll.h>
#include <signal.h>
#include <unistd.h>

#include "nioev/lib/Enums.hpp"
#include "ApplicationState.hpp"

namespace nioev::mqtt {

using namespace nioev::lib;

ClientThreadManager::ClientThreadManager(ApplicationState& app)
: mApp(app) {
    mEpollFd = epoll_create1(EPOLL_CLOEXEC);
    if(mEpollFd < 0) {
        spdlog::critical("Failed to create epoll fd: " + errnoToString());
        exit(5);
    }
    uint threadCount = std::max<uint>(4, std::thread::hardware_concurrency() / 2);
    for(uint i = 0; i < threadCount; ++i) {
        mReceiverThreads.emplace_back([this, i] {
            std::string threadName = "C-" + std::to_string(i);
            pthread_setname_np(pthread_self(), threadName.c_str());
            sigset_t blockedSignals = { 0 };
            sigemptyset(&blockedSignals);
            sigaddset(&blockedSignals, SIGUSR1);
            pthread_sigmask(SIG_BLOCK, &blockedSignals, nullptr);
            receiverThreadFunction();
        });
    }
}
void ClientThreadManager::receiverThreadFunction() {
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
                suspendLock.unlock();
                mSuspendMutex2.lock_shared();
                mSuspendMutex2.unlock_shared();
                suspendLock.lock();
                continue;
            }
            spdlog::warn("epoll_wait(): {}", errnoToString());
            continue;
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
                    while(!sendTasks.empty()) {
                        auto& task = sendTasks.front();
                        // send data
                        uint bytesSend = 0;
                        do {
                            bytesSend = client.getTcpClient().send(task.bytes.data() + task.offset, task.bytes.size() - task.offset);
                            task.offset += bytesSend;
                        } while(bytesSend > 0 && task.offset < task.bytes.size());

                        if(task.offset >= task.bytes.size()) {
                            sendTasks.pop();
                            if(client.getState() == MQTTClientConnection::ConnectionState::INVALID_PROTOCOL_VERSION) {
                                assert(sendTasks.empty());
                                throw CleanDisconnectException{};
                            }
                        } else {
                            break;
                        }

                    }
                }
                if(events[i].events & (EPOLLIN | EPOLLHUP)) {
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
                        if(bytesReceived > 0) {
                            client.setLastDataRecvTimestamp(std::chrono::steady_clock::now().time_since_epoch().count());
                        }
                        for(uint i = 0; i < bytesReceived;) {
                            switch(recvData.recvState) {
                            case MQTTClientConnection::PacketReceiveState::IDLE: {
                                if(bytes.at(i) == 'S' && client.getState() == MQTTClientConnection::ConnectionState::INITIAL) {
                                    // simplified protocol, handled by the scripting engine
                                    auto[_, sendLock] = client.getSendTasks();
                                    std::vector<uint8_t> toMove{bytes.begin(), bytes.begin() + bytesReceived};
                                    if(epoll_ctl(mEpollFd, EPOLL_CTL_DEL, events[i].data.fd, nullptr) < 0) {
                                        spdlog::warn("epoll_ctl(EPOLL_CTL_DEL) failed: {}", lib::errnoToString());
                                    }
                                    // FIXME scripting
                                    //mApp.passTcpClientToScriptingEngine(client.moveTcpClient(), std::move(toMove));
                                    throw CleanDisconnectException{};
                                }
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
                                        handlePacketReceived(client, recvData, recvDataRefLock);
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
                                    handlePacketReceived(client, recvData, recvDataRefLock);
                                    recvData.recvState = MQTTClientConnection::PacketReceiveState::IDLE;
                                }
                                break;
                            }
                            }
                        }
                    } while(bytesReceived > 0);
                }
            } catch(CleanDisconnectException&) {
                mApp.requestChange(ChangeRequestLogoutClient{client.makeShared()});
            } catch(std::exception& e) {
                spdlog::error("Caught: {}", e.what());
                mApp.requestChange(ChangeRequestLogoutClient{client.makeShared()});
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
}
void ClientThreadManager::handlePacketReceived(MQTTClientConnection& client, const MQTTClientConnection::PacketReceiveData& recvData, std::unique_lock<std::mutex>& clientReceiveLock) {
    spdlog::debug("Received packet of type {}", recvData.messageType);

    BinaryDecoder decoder{recvData.currentReceiveBuffer, recvData.packetLength};
    switch(client.getState()) {
    case MQTTClientConnection::ConnectionState::INITIAL: {
        switch(recvData.messageType) {
        case MQTTMessageType::CONNECT: {
            // initial connect
            constexpr uint8_t protocolName[] = { 0, 4, 'M', 'Q', 'T', 'T' };
            if(memcmp(protocolName, decoder.getCurrentPtr(), 6) != 0) {
                protocolViolation("Invalid protocol version");
            }
            decoder.advance(6);

            uint8_t protocolLevel = decoder.decodeByte();
            if(protocolLevel != 4) {
                // we only support MQTT 3.1.1
                BinaryEncoder response;
                response.encodeByte(static_cast<uint8_t>(MQTTMessageType::CONNACK) << 4);
                response.encodeByte(2); // remaining packet length
                response.encodeByte(0); // no session present
                response.encodeByte(1); // invalid protocol version
                client.setState(MQTTClientConnection::ConnectionState::INVALID_PROTOCOL_VERSION);
                spdlog::error("Invalid protocol version requested by client: {}", protocolLevel);
                client.sendData(response.moveData());
                break;
            }
            uint8_t connectFlags = decoder.decodeByte();
            uint16_t keepAlive = decoder.decode2Bytes();
            client.setKeepAliveIntervalSeconds(keepAlive);
            if(connectFlags & 0x1) {
                protocolViolation("Invalid connect flags bit set");
            }
            bool cleanSession = connectFlags & 0x2;
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
            // according to the MQTT spec, a client can actually send PUBLISH before receiving CONNACK, so as logging a client in is handled async, we need to set the connected flag here already
            client.setState(MQTTClientConnection::ConnectionState::CONNECTED);
            mApp.requestChange(ChangeRequestLoginClient{client.makeShared(), std::move(clientId), cleanSession ? CleanSession::Yes : CleanSession::No});

            break;
        }
        default: {
            protocolViolation("Packet not allowed here");
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
                protocolViolation("Invalid QoS");
            }
            QoS qos = static_cast<QoS>(qosInt);
            auto retain = static_cast<Retain>(!!(recvData.firstByte & 0x1));
            auto topic = decoder.decodeString(); // TODO check for allowed chars
            if(topic.empty()) {
                protocolViolation("Invalid topic");
            }
            if(qos == QoS::QoS1 || qos == QoS::QoS2) {
                auto id = decoder.decode2Bytes();
                if(qos == QoS::QoS1) {
                    // send PUBACK
                    BinaryEncoder encoder;
                    encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::PUBACK) << 4);
                    encoder.encode2Bytes(id);
                    encoder.insertPacketLength();
                    client.sendData(encoder.moveData());
                } else {
                    // send PUBREC
                    BinaryEncoder encoder;
                    encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::PUBREC) << 4);
                    encoder.encode2Bytes(id);
                    encoder.insertPacketLength();
                    client.sendData(encoder.moveData());
                    auto[state, stateLock] = client.getPersistentState();
                    state->qos2receivingPacketIds[id] = true;
                }
            }
            std::vector<uint8_t> data = decoder.getRemainingBytes();
            mApp.publish(std::move(topic), std::move(data), qos, retain);
            break;
        }
        case MQTTMessageType::PUBACK: {
            uint16_t id = decoder.decode2Bytes();
            auto[state, lock] = client.getPersistentState();
            state->qos1sendingPackets.erase(id);
            break;
        }
        case MQTTMessageType::PUBREL: {
            // QoS 2 part 2
            if(recvData.firstByte != 0x62) {
                protocolViolation("Invalid first byte in PUBREL");
            }
            uint16_t id = decoder.decode2Bytes();
            {
                auto[state, stateLock] = client.getPersistentState();
                if(!state->qos2receivingPacketIds[id]) {
                    stateLock.unlock();
                    spdlog::warn("[{}] PUBREL no such message id", client.getClientId());
                } else {
                    state->qos2receivingPacketIds[id] = false;
                }
            }

            // send PUBCOMP
            BinaryEncoder encoder;
            encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::PUBCOMP) << 4);
            encoder.encode2Bytes(id);
            encoder.insertPacketLength(); // TODO prevent unnecessary calls like this by precomputing payload length
            client.sendData(encoder.moveData());
            break;
        }
        case MQTTMessageType::PUBREC: {
            // QoS 2 part 1 (sending packets)
            uint16_t id = decoder.decode2Bytes();
            {
                auto[state, stateLock] = client.getPersistentState();
                auto erased = state->qos2sendingPackets.erase(id);
                if(erased != 1) {
                    stateLock.unlock();
                    spdlog::warn("[{}] PUBREC no such message id (erased {})", client.getClientId(), erased);
                }
                state->qos2pubrecReceived[id] = true;
            }

            // send PUBREL
            BinaryEncoder encoder;
            encoder.encodeByte((static_cast<uint8_t>(MQTTMessageType::PUBREL) << 4) | 0b10);
            encoder.encode2Bytes(id);
            encoder.insertPacketLength();
            client.sendData(encoder.moveData());
            break;
        }
        case MQTTMessageType::PUBCOMP: {
            // QoS 2 part 3 (sending packets)
            uint16_t id = decoder.decode2Bytes();
            auto[state, stateLock] = client.getPersistentState();
            state->qos2pubrecReceived[id] = false;
            break;
        }
        case MQTTMessageType::SUBSCRIBE: {
            if(recvData.firstByte != 0x82) {
                protocolViolation("SUBSCRIBE invalid first byte");
            }
            auto packetIdentifier = decoder.decode2Bytes();


            BinaryEncoder encoder;
            encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::SUBACK) << 4);
            encoder.encode2Bytes(packetIdentifier);
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
                mApp.requestChange(makeChangeRequestSubscribe(client.makeShared(), std::move(topic), qos));
            } while(!decoder.empty());


            encoder.insertPacketLength();
            client.sendData(encoder.moveData());

            break;
        }
        case MQTTMessageType::UNSUBSCRIBE: {
            if(recvData.firstByte != 0xA2) {
                protocolViolation("UNSUBSCRIBE invalid first byte");
            }
            auto packetIdentifier = decoder.decode2Bytes();
            do {
                auto topic = decoder.decodeString();
                mApp.requestChange(ChangeRequestUnsubscribe{client.makeShared(), topic});
            } while(!decoder.empty());

            // prepare SUBACK
            BinaryEncoder encoder;
            encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::UNSUBACK) << 4);
            encoder.encode2Bytes(packetIdentifier);
            encoder.insertPacketLength();
            client.sendData(encoder.moveData());

            break;
        }
        case MQTTMessageType::PINGREQ: {
            if(recvData.firstByte != 0xC0) {
                protocolViolation("PINGREQ Invalid first byte");
            }
            BinaryEncoder encoder;
            encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::PINGRESP) << 4);
            encoder.insertPacketLength();
            client.sendData(encoder.moveData());
            spdlog::debug("Replying to PINGREQ");
            break;
        }
        case MQTTMessageType::DISCONNECT: {
            if(recvData.firstByte != 0xE0) {
                protocolViolation("Invalid disconnect first byte");
            }
            spdlog::info("[{}] Received disconnect request", client.getClientId());
            client.discardWill();
            throw CleanDisconnectException{};

            break;
        }
        }
        break;
    }
    }
}
void ClientThreadManager::protocolViolation(const std::string& reason) {
    throw std::runtime_error{"Protocol violation: " + reason};
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
}

}
