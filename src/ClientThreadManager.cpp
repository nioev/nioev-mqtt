#include "ClientThreadManager.hpp"
#include "Util.hpp"
#include <spdlog/spdlog.h>
#include <sys/epoll.h>
#include <signal.h>
#include <unistd.h>

#include "Enums.hpp"
#include "ApplicationState.hpp"
#include "Util.hpp"

namespace nioev {

ClientThreadManager::ClientThreadManager(ApplicationState& app, uint threadCount)
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
            sigset_t blockedSignals = { 0 };
            sigemptyset(&blockedSignals);
            sigaddset(&blockedSignals, SIGUSR1);
            pthread_sigmask(SIG_BLOCK, &blockedSignals, nullptr);
            receiverThreadFunction();
        });
    }
}
void ClientThreadManager::receiverThreadFunction() {
    auto suspendIfRequested = [this] {
        if(mShouldSuspend) {
            mSuspendedThreads.fetch_add(1);
            while(mShouldSuspend) {
                std::this_thread::yield();
            }
            mSuspendedThreads.fetch_sub(1);
        }
    };
    sigset_t blockedSignalsDuringEpoll = { 0 };
    sigemptyset(&blockedSignalsDuringEpoll);
    sigaddset(&blockedSignalsDuringEpoll, SIGINT);
    sigaddset(&blockedSignalsDuringEpoll, SIGTERM);
    std::vector<uint8_t> bytes;
    bytes.resize(64 * 1024 * 4);
    while(!mShouldQuit) {
        suspendIfRequested();
        epoll_event events[128] = { 0 };
        int eventCount = epoll_pwait(mEpollFd, events, 128, -1, &blockedSignalsDuringEpoll);
        if(eventCount < 0) {
            if(errno == EINTR) {
                suspendIfRequested();
                continue;
            }
            spdlog::warn("epoll_wait(): {}", util::errnoToString());
            continue;
        }
        for(int i = 0; i < eventCount; ++i) {
            auto& client = * (MQTTClientConnection*)events[i].data.ptr;
            try {
                if(events[i].events & EPOLLERR) {
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
                            bytesSend = client.getTcpClient().send(task.data.data() + task.offset, task.data.size() - task.offset);
                            task.offset += bytesSend;
                        } while(bytesSend > 0 && task.offset < task.data.size());

                        if(task.offset >= task.data.size()) {
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
                    // This check might seem superfluous, because we can't even get a reference if the client is already disconnected. We do this any-
                    // way because there is actually no lock required for setting a client to disconnected, so this is a race. In that case
                    // TcpClient::recv further down would just fail with bad fd, but that spams the logs, so we do an additional check here so that in
                    // 99.99% of cases we should not get wrong logs. Without employing even more mutexes (and we already have too many in my mind) there
                    // is probably no way to prevent this.
                    if(client.isLoggedOut())
                        throw CleanDisconnectException{};
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
                                        spdlog::warn("epoll_ctl(EPOLL_CTL_DEL) failed: {}", util::errnoToString());
                                    }
                                    // FIXME scripting
                                    //mApp.passTcpClientToScriptingEngine(client.moveTcpClient(), std::move(toMove));
                                    throw CleanDisconnectException{};
                                }
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
                mApp.requestChange(ChangeRequestDisconnectClient{client.makeShared()});
            } catch(std::exception& e) {
                spdlog::error("Caught: {}", e.what());
                mApp.requestChange(ChangeRequestDisconnectClient{client.makeShared()});
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
        spdlog::critical("Failed to add fd to epoll: {}", util::errnoToString());
        exit(6);
    }
}
void ClientThreadManager::removeClientConnection(MQTTClientConnection& conn) {
    if(epoll_ctl(mEpollFd, EPOLL_CTL_DEL, conn.getTcpClient().getFd(), nullptr) < 0) {
        spdlog::debug("Failed to remove fd from epoll: {}", util::errnoToString());
    }
}
void ClientThreadManager::handlePacketReceived(MQTTClientConnection& client, const MQTTClientConnection::PacketReceiveData& recvData, std::unique_lock<std::mutex>& clientReceiveLock) {
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

            uint8_t protocolLevel = decoder.decodeByte();
            if(protocolLevel != 4) {
                // we only support MQTT 3.1.1
                std::vector<uint8_t> response;
                response.push_back(static_cast<uint8_t>(MQTTMessageType::CONNACK) << 4);
                response.push_back(2); // remaining packet length
                response.push_back(0); // no session present
                response.push_back(1); // invalid protocol version
                client.setState(MQTTClientConnection::ConnectionState::INVALID_PROTOCOL_VERSION);
                spdlog::error("Invalid protocol version requested by client: {}", protocolLevel);
                client.sendData(std::move(response));
                break;
            }
            uint8_t connectFlags = decoder.decodeByte();
            uint16_t keepAlive = decoder.decode2Bytes();
            client.setKeepAliveIntervalSeconds(keepAlive);
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
                if(willTopic.empty()) {
                    protocolViolation();
                }
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
            // FIXME ensure that we don't receive packets while logging in
            mApp.requestChange(ChangeRequestLoginClient{client.makeShared(), std::move(clientId), cleanSession ? CleanSession::Yes : CleanSession::No});

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
            if(topic.empty()) {
                protocolViolation();
            }
            if(qos == QoS::QoS1 || qos == QoS::QoS2) {
                auto id = decoder.decode2Bytes();
                if(qos == QoS::QoS1) {
                    // send PUBACK
                    util::BinaryEncoder encoder;
                    encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::PUBACK) << 4);
                    encoder.encode2Bytes(id);
                    encoder.insertPacketLength();
                    client.sendData(encoder.moveData());
                } else {
                    // send PUBREC
                    util::BinaryEncoder encoder;
                    encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::PUBREC) << 4);
                    encoder.encode2Bytes(id);
                    encoder.insertPacketLength();
                    client.sendData(encoder.moveData());
                    auto[state, stateLock] = client.getPersistentState();
                    if(state->qos3receivingPacketIds.contains(id)) {
                        break;
                    }
                    state->qos3receivingPacketIds.emplace(id);
                }
            }
            std::vector<uint8_t> data = decoder.getRemainingBytes();
            // we need to go through the app to 1. save retained messages 2. interact with scripts
            mApp.publish(std::move(topic), std::move(data), qos, retain);
            break;
        }
        case MQTTMessageType::PUBREL: {
            // QoS 2 part 2
            if(recvData.firstByte != 0x62) {
                protocolViolation();
            }
            uint16_t id = decoder.decode2Bytes();
            auto[state, stateLock] = client.getPersistentState();
            auto numErased = state->qos3receivingPacketIds.erase(id);
            assert(numErased == 1);

            // send PUBCOMP
            util::BinaryEncoder encoder;
            encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::PUBCOMP) << 4);
            encoder.encode2Bytes(id);
            encoder.insertPacketLength();
            client.sendData(encoder.moveData());
            break;
        }
        case MQTTMessageType::SUBSCRIBE: {
            if(recvData.firstByte != 0x82) {
                protocolViolation();
            }
            auto packetIdentifier = decoder.decode2Bytes();
            do {
                auto topic = decoder.decodeString();
                if(topic.empty()) {
                    protocolViolation();
                }
                spdlog::info("[{}] Subscribing to {}", client.getClientId(), topic);
                uint8_t qosInt = decoder.decodeByte();
                if(qosInt >= 3) {
                    protocolViolation();
                }
                auto qos = static_cast<QoS>(qosInt);
                auto topicSplit = util::splitTopics(topic);
                auto hasWildcard = util::hasWildcard(topic);
                mApp.requestChange(ChangeRequestSubscribe{client.makeShared(), std::move(topic), std::move(topicSplit), hasWildcard ? SubscriptionType::WILDCARD : SubscriptionType::SIMPLE, qos});
            } while(!decoder.empty());


            // prepare SUBACK
            util::BinaryEncoder encoder;
            encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::SUBACK) << 4);
            encoder.encode2Bytes(packetIdentifier);
            encoder.encodeByte(0); // maximum QoS 0 TODO support more
            encoder.insertPacketLength();
            client.sendData(encoder.moveData());

            break;
        }
        case MQTTMessageType::UNSUBSCRIBE: {
            if(recvData.firstByte != 0xA2) {
                protocolViolation();
            }
            auto packetIdentifier = decoder.decode2Bytes();
            do {
                auto topic = decoder.decodeString();
                mApp.requestChange(ChangeRequestUnsubscribe{client.makeShared(), topic});
            } while(!decoder.empty());

            // prepare SUBACK
            util::BinaryEncoder encoder;
            encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::UNSUBACK) << 4);
            encoder.encode2Bytes(packetIdentifier);
            encoder.insertPacketLength();
            client.sendData(encoder.moveData());

            break;
        }
        case MQTTMessageType::PINGREQ: {
            if(recvData.firstByte != 0xC0) {
                protocolViolation();
            }
            util::BinaryEncoder encoder;
            encoder.encodeByte(static_cast<uint8_t>(MQTTMessageType::PINGRESP) << 4);
            encoder.insertPacketLength();
            client.sendData(encoder.moveData());
            spdlog::debug("Replying to PINGREQ");
            break;
        }
        case MQTTMessageType::DISCONNECT: {
            if(recvData.firstByte != 0xE0) {
                protocolViolation();
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
void ClientThreadManager::protocolViolation() {
    throw std::runtime_error{"Protocol violation"};
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
void ClientThreadManager::suspendAllThreads() {
    mShouldSuspend = true;
    for(auto& thread: mReceiverThreads) {
        pthread_kill(thread.native_handle(), SIGUSR1);
    }
    while(mSuspendedThreads != mReceiverThreads.size()) {
        std::this_thread::yield();
    }
}
void ClientThreadManager::resumeAllThreads() {
    mShouldSuspend = false;
    while(mSuspendedThreads != 0) {
        std::this_thread::yield();
    }
}

}
