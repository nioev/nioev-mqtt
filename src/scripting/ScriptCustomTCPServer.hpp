#pragma once

#include "../Forward.hpp"
#include "../TcpClientConnection.hpp"
#include "../Enums.hpp"

#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <cstdint>
#include <queue>

namespace nioev {


class ScriptCustomTCPServer {
public:
    explicit ScriptCustomTCPServer(ScriptContainerManager& scriptManager);
    ~ScriptCustomTCPServer();
    void scriptListen(std::string script, std::string listenIdentifier, Compression sendCompression, Compression recvCompression);
    void notifyScriptDied(const std::string& script);
    void sendMsgFromScript(const std::string& script, int targetFd, std::vector<uint8_t>&& msg);
    void passTcpClient(TcpClientConnection&& tcpClient, std::vector<uint8_t>&& receivedData);
private:
    struct StoredTcpClient {
        TcpClientConnection tcpClient;
        std::string script;
        StoredTcpClient(TcpClientConnection&& client, std::string script)
        : tcpClient(std::move(client)), script(std::move(script)) {

        }
        enum class RecvState {
            RECEIVING_S,
            RECEIVING_LENGTH,
            RECEIVING_DATA
        };
        size_t recvLen = 0;
        std::vector<uint8_t> recvBuffer;
        RecvState recvState = RecvState::RECEIVING_S;
        bool isFirstMsg = true;
        Compression sendCompression = Compression::NONE, recvCompression = Compression::NONE;

        std::queue<std::pair<std::vector<uint8_t>, size_t>> sendTasks;
    };
    void secondThreadFunc();
    void handleDataReceived(StoredTcpClient&, const std::vector<uint8_t>& bytes, size_t bytesSize);

    void sendData(StoredTcpClient& client, const uint8_t* data, size_t dataLen, Compression compression);
    void deleteClient(std::unordered_map<int, StoredTcpClient>::iterator itr);

    std::mutex mMutex;
    int mEpollFd = -1;
    ScriptContainerManager& mScriptManager;
    std::unordered_map<int, StoredTcpClient> mTcpClients;
    struct Listener {
        std::string script;
        Compression sendCompression, recvCompression;
        Listener(std::string&& script, Compression sendCompression, Compression recvCompression)
        : script(std::move(script)), sendCompression(sendCompression), recvCompression(recvCompression) {

        }
    };
    std::unordered_map<std::string, Listener> mListeningScripts;

    std::atomic<bool> mShouldRun = true;
    std::thread mThread;
};

}