#include "MQTTClientConnection.hpp"
#include "ApplicationState.hpp"
#include "Util.hpp"
#include "MQTTPublishPacketBuilder.hpp"

namespace nioev {

void MQTTClientConnection::publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained, MQTTPublishPacketBuilder& packetBuilder) {
    auto packet = packetBuilder.getPacket(qos);
    if(qos == QoS::QoS1){
        auto[persistentState, lock] = getPersistentState();
        persistentState->qos1sendingPackets.emplace(mPublishPacketId - 1, packet);
    }
    sendData(std::move(packet));
}

void MQTTClientConnection::sendData(util::SharedBuffer&& bytes) {
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
        // we aren't allowed to enqueue a change request here, because we could be inside ApplicationState::publish, where a shared lock is held.
        // that's why we just set a flag which causes the logout to be enequeued later on
        mSendError = true;
    }
}

}