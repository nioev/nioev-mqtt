#include "MQTTClientConnection.hpp"
#include "ApplicationState.hpp"

namespace nioev {

void MQTTClientConnection::sendData(std::vector<uint8_t>&& bytes) {
    try {
        uint totalBytesSent = 0;
        uint bytesSent = 0;
        auto [sendTasksRef, sendLock] = getSendTasks();
        auto& sendTasks = sendTasksRef.get();
        if(sendTasks.empty()) {
            do {
                bytesSent = getTcpClient().send(bytes.data() + totalBytesSent, bytes.size() - totalBytesSent);
                totalBytesSent += bytesSent;
            } while(bytesSent > 0 && totalBytesSent < bytes.size());
        }
        //spdlog::warn("Bytes sent: {}, Total bytes sent: {}", bytesSent, totalBytesSent);

        if(totalBytesSent < bytes.size()) {
            // TODO implement maximum queue depth
            sendTasks.emplace(MQTTClientConnection::SendTask{ std::move(bytes), totalBytesSent });
        }
    } catch(std::exception& e) {
        spdlog::error("Error while sending data: {}", e.what());
        mApp.requestChange(ChangeRequestLogoutClient{makeShared()});
    }
}

}