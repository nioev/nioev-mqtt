#include "ScriptCustomTCPServer.hpp"
#include "../TcpClientConnection.hpp"
#include "ScriptContainerManager.hpp"
#include <sys/epoll.h>
#include <sys/signal.h>
#include <zstd.h>

namespace nioev {

ScriptCustomTCPServer::ScriptCustomTCPServer(ScriptContainerManager& scriptManager)
: mScriptManager(scriptManager), mEpollFd(epoll_create1(EPOLL_CLOEXEC)), mThread([this]{secondThreadFunc();}) {

}
ScriptCustomTCPServer::~ScriptCustomTCPServer() {
    close(mEpollFd);
    mEpollFd = -1;
    mShouldRun = false;
    pthread_kill(mThread.native_handle(), SIGUSR1);
    mThread.join();
    spdlog::info("Safely stopped script tcp server");
}
void ScriptCustomTCPServer::secondThreadFunc() {
    pthread_setname_np(pthread_self(), "script-tcp");
    std::array<epoll_event, 1024> events;
    std::vector<uint8_t> recvBuffer;
    recvBuffer.resize(64 * 1024);
    while(mShouldRun) {
        int eventCount = epoll_wait(mEpollFd, events.data(), events.size(), -1);
        if(eventCount < 0) {
            spdlog::warn("epoll_wait(): {}", util::errnoToString());
            continue;
        }
        std::unique_lock<std::mutex> lock{mMutex};
        for(int i = 0; i < eventCount; ++i) {
            auto client = mTcpClients.find(events[i].data.fd);
            if(client == mTcpClients.end()) {
                spdlog::warn("Received an event ({}) but TcpClient isn't in the map!", (unsigned)events[i].events);
                continue;
            }
            try {
                if(events[i].events & EPOLLERR) {
                    throw CleanDisconnectException{};
                }
                if(events[i].events & EPOLLIN) {
                    size_t receivedBytesCount;
                    do {
                        receivedBytesCount = client->second.tcpClient.recv(recvBuffer);
                        handleDataReceived(client->second, recvBuffer, receivedBytesCount);
                    } while(receivedBytesCount > 0);
                }
                if(events[i].events & EPOLLOUT) {
                    size_t bytesSent = 0;
                    while(!client->second.sendTasks.empty()) {
                        auto& task = client->second.sendTasks.front();
                        bytesSent = client->second.tcpClient.send(task.first.data() + task.second, task.first.size() - task.second);
                        task.second += bytesSent;
                        if(task.second >= task.first.size()) {
                            client->second.sendTasks.pop();
                        }
                        if(bytesSent == 0) {
                            break;
                        }
                    }
                }
            } catch(CleanDisconnectException&) {
                deleteClient(client);
            } catch(std::exception& e) {
                deleteClient(client);
                spdlog::error("Caught: {}", e.what());
            }
        }
    }
}
void ScriptCustomTCPServer::scriptListen(std::string script, std::string listenIdentifier, Compression sendCompression, Compression recvCompression) {
    std::unique_lock<std::mutex> lock{mMutex};
    mListeningScripts.emplace(std::piecewise_construct, std::make_tuple(std::move(listenIdentifier)), std::make_tuple(std::move(script), sendCompression, recvCompression));
}
void ScriptCustomTCPServer::notifyScriptDied(const std::string& script) {
    std::unique_lock<std::mutex> lock{mMutex};
    for(auto it = mTcpClients.begin(); it != mTcpClients.end();) {
        if(it->second.script == script) {
            it = mTcpClients.erase(it);
        } else {
            it++;
        }
    }
    for(auto it = mListeningScripts.begin(); it != mListeningScripts.end();) {
        if(it->second.script == script) {
            it = mListeningScripts.erase(it);
        } else {
            it++;
        }
    }
}
void ScriptCustomTCPServer::passTcpClient(TcpClientConnection&& tcpClient, std::vector<uint8_t>&& receivedData) {
    std::unique_lock<std::mutex> lock{mMutex};
    auto fd = tcpClient.getFd();
    {
        // fd was repurposed before we were notified :c
        auto existingClient = mTcpClients.find(fd);
        if(existingClient != mTcpClients.end()) {
            deleteClient(existingClient);
        }
    }
    spdlog::info("[{}:{}] Passed TcpClient connection to ScriptCustomTCPServer", tcpClient.getRemoteIp(), tcpClient.getRemotePort());
    auto client = mTcpClients.emplace(std::piecewise_construct, std::make_tuple(fd), std::make_tuple(std::move(tcpClient), ""));
    epoll_event ev = { 0 };
    ev.data.fd = fd;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLEXCLUSIVE | EPOLLET;
    if(epoll_ctl(mEpollFd, EPOLL_CTL_ADD, fd, &ev) < 0) {
        spdlog::warn("epoll_ctl(EPOLL_CTL_ADD) failed: {}", util::errnoToString());
    }
    handleDataReceived(client.first->second, receivedData, receivedData.size());
}
void ScriptCustomTCPServer::handleDataReceived(StoredTcpClient& client, const std::vector<uint8_t>& bytes, size_t bytesSize) {
    for(size_t i = 0; i < bytesSize; ++i) {
        switch(client.recvState) {
        case StoredTcpClient::RecvState::RECEIVING_S:
            if(bytes.at(i) != 'S') {
                throw std::runtime_error("Simplified protocol violation! 1");
            }
            client.recvState = StoredTcpClient::RecvState::RECEIVING_LENGTH;
            client.recvLen = 0;
            client.recvBuffer = {};
            break;
        case StoredTcpClient::RecvState::RECEIVING_LENGTH:
            if(bytes.at(i) >= '0' && bytes.at(i) <= '9') {
                client.recvLen *= 10;
                client.recvLen += bytes.at(i) - '0';
            } else if(bytes.at(i) == ':') {
                client.recvState = StoredTcpClient::RecvState::RECEIVING_DATA;
            } else {
                throw std::runtime_error(std::string{"Simplified protocol violation! "} + (char)bytes.at(i));
            }
            break;
        case StoredTcpClient::RecvState::RECEIVING_DATA:
            client.recvBuffer.push_back(bytes[i]);
            if(client.recvLen <= client.recvBuffer.size()) {
                if(client.isFirstMsg) {
                    std::string recvStr{client.recvBuffer.begin(), client.recvBuffer.end()};
                    auto listeningScript = mListeningScripts.find(recvStr);
                    if(listeningScript == mListeningScripts.end()) {
                        sendData(client, reinterpret_cast<const uint8_t*>("ERROR"), 5, Compression::NONE);
                        throw CleanDisconnectException{};
                    }
                    client.script = listeningScript->second.script;
                    client.sendCompression = listeningScript->second.sendCompression;
                    client.recvCompression = listeningScript->second.recvCompression;

                    client.isFirstMsg = false;
                    mScriptManager.runScript(client.script, ScriptRunArgsTcpNewClient{client.tcpClient.getFd()}, {});
                } else {
                    mScriptManager.runScript(client.script, ScriptRunArgsTcpNewDataFromClient{client.tcpClient.getFd(), std::move(client.recvBuffer)}, {});
                    client.recvState = StoredTcpClient::RecvState::RECEIVING_S;
                    client.recvBuffer = {};
                }
            }
            break;
        }
    }
}
void ScriptCustomTCPServer::sendData(StoredTcpClient& client, const uint8_t* data, size_t dataLen, Compression compression) {
    try {
        std::vector<uint8_t> buffer;
        buffer.push_back('S');

        if(compression == Compression::ZSTD) {
            auto zstdReserved = ZSTD_compressBound(dataLen);
            std::vector<uint8_t> compressed;
            compressed.resize(zstdReserved);
            auto compressedSize = ZSTD_compress(compressed.data(), compressed.size(), data, dataLen, 3);

            std::string lenAsStr = std::to_string(compressedSize);
            buffer.insert(buffer.end(), lenAsStr.begin(), lenAsStr.end());
            buffer.push_back(':');

            buffer.insert(buffer.end(), compressed.begin(), compressed.begin() + compressedSize);
        } else {
            std::string lenAsStr = std::to_string(dataLen);
            buffer.insert(buffer.end(), lenAsStr.begin(), lenAsStr.end());
            buffer.push_back(':');
            buffer.insert(buffer.end(), data, data + dataLen);
        }

        size_t bytesSent = 0, bytesSentThisTime;
        if(client.sendTasks.empty()) {
            do {
                bytesSentThisTime = client.tcpClient.send(buffer.data() + bytesSent, buffer.size() - bytesSent);
                bytesSent += bytesSentThisTime;
            } while(bytesSent < buffer.size() && bytesSentThisTime > 0);
        }
        if(bytesSent < buffer.size()) {
            client.sendTasks.emplace(std::make_pair(std::move(buffer), bytesSent));
        }
    } catch(std::exception& e) {
        spdlog::warn("Error sending data: {}", e.what());
        deleteClient(mTcpClients.find(client.tcpClient.getFd()));
    }
}
void ScriptCustomTCPServer::deleteClient(std::unordered_map<int, ScriptCustomTCPServer::StoredTcpClient>::iterator client) {
    if(client == mTcpClients.end())
        return;
    spdlog::info("[{}:{}] Deleting client from ScriptCustomTCPServer previously connected to {}", client->second.tcpClient.getRemoteIp(), client->second.tcpClient.getRemotePort(), client->second.script);
    if(!client->second.script.empty()) {
        mScriptManager.runScript(client->second.script, ScriptRunArgsTcpDeleteClient{client->second.tcpClient.getFd()}, {});
    }
    mTcpClients.erase(client);
}
void ScriptCustomTCPServer::sendMsgFromScript(const std::string& script, int targetFd, std::vector<uint8_t>&& msg) {
    auto client = mTcpClients.find(targetFd);
    if(client == mTcpClients.end()) {
        spdlog::warn("Failed to send data from script, TcpClient doesn't exist anymore!");
        return;
    }
    if(client->second.script != script) {
        spdlog::error("Script {} tried to send msg to fd {}, which it doesn't own!", script, targetFd);
        return;
    }
    sendData(client->second, msg.data(), msg.size(), client->second.sendCompression);
}

}