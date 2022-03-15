#include "MQTTClientConnection.hpp"
#include "ApplicationState.hpp"
#include "Util.hpp"
#include "MQTTPublishPacketBuilder.hpp"

namespace nioev {

void MQTTClientConnection::publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained, MQTTPublishPacketBuilder& packetBuilder) {
    auto packet = packetBuilder.getPacket(qos);
    uint16_t packetId = 0;
    if(qos == QoS::QoS1 || qos == QoS::QoS2) {
        packetId = packet.getPacketId();
    }
    if(sendData(std::move(packet), SendDataType::PUBLISH)) {
        if(qos == QoS::QoS1) {
            auto[persistentState, lock] = getPersistentState();
            persistentState->qos1sendingPackets.emplace(packetId, packet);
        } else if(qos == QoS::QoS2) {
            auto[persistentState, lock] = getPersistentState();
            persistentState->qos2sendingPackets.emplace(packetId, packet);
        }
    }
}

bool MQTTClientConnection::sendData(util::SharedBuffer&& bytes, SendDataType type) {
    try {
        uint totalBytesSent = 0;
        uint bytesSent = 0;

        std::unique_lock<std::mutex> lock{mSendMutex/*, std::defer_lock*/};
        /*if(type == SendDataType::DEFAULT) {
            lock.lock();
        } else {
            // TODO make configurable
            if(!lock.try_lock_for(std::chrono::microseconds(100))) {
                spdlog::warn("[{}] Dropping message due to inability to acquire lock within 1ms", mClientId);
                return false;
            }
        }*/
        //lock.lock();

        // TODO make configurable
        if(type == SendDataType::PUBLISH && mSendTasks.size() > 1000) {
            //spdlog::warn("[{}] Dropping packet due to large queue depth", mClientId);
            return false;
        }
        if(mSendTasks.empty()) {
            do {
                bytesSent = getTcpClient().send(bytes.data() + totalBytesSent, bytes.size() - totalBytesSent);
                totalBytesSent += bytesSent;
            } while(bytesSent > 0 && totalBytesSent < bytes.size());
        }
        //spdlog::warn("Bytes sent: {}, Total bytes sent: {}", bytesSent, totalBytesSent);

        if(totalBytesSent < bytes.size()) {
            mSendTasks.emplace(MQTTClientConnection::SendTask{ std::move(bytes), totalBytesSent });
        }
    } catch(std::exception& e) {
        spdlog::error("[{}] Error while sending data: {}", getClientId(), e.what());
        // we aren't allowed to enqueue a change request here, because we could be inside ApplicationState::publish, where a shared lock is held.
        // that's why we just set a flag which causes the logout to be enequeued later on
        mSendError = true;
    }
    return true;
}

}