#include "Statistics.hpp"
#include "ApplicationState.hpp"
#include "MQTTPublishPacketBuilder.hpp"
#include "StatisticsConverter.hpp"
#include "Timers.hpp"

namespace nioev {

Statistics::Statistics(ApplicationState& app)
: mApp(app) {
    mBatchAnalysisTimer.addPeriodicTask(std::chrono::minutes(1), [this] {
        std::unique_lock<std::shared_mutex> lock{mMutex};
        refreshInternal();
    });
    mSampleWorkerThreadTimer.addPeriodicTask(std::chrono::milliseconds(20), [this] {
        auto currentSleepLevel = mApp.getCurrentWorkerThreadSleepLevel();
        {
            std::unique_lock<std::shared_mutex> lock{mMutex};
            auto nowRounded = std::chrono::floor<std::chrono::minutes>(std::chrono::system_clock::now());
            ensureEnoughSpace<std::chrono::minutes, 60>(mSleepLevelSampleCounts, nowRounded);
            mSleepLevelSampleCounts.back().samples.at(static_cast<int>(currentSleepLevel)) += 1;
        }
    });
    mStartTime = std::chrono::steady_clock::now();
}
void Statistics::init() {
    mApp.requestChange(ChangeRequestSubscribe{makeShared(), "", {}, SubscriptionType::OMNI, QoS::QoS2});

    // TODO move ui logic to nioev-scripting?
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt", util::stringToBuffer("{}"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats", util::stringToBuffer(R"({"type": "grid"})"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/99_pinger", util::stringToBuffer(R"({"type": "pinger", "interval_ms": 15000, "topic": "$NIOEV/request_new_stats"})"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/01_msg_per_second", util::stringToBuffer(R"({"type": "graph", "headline": "Messages per Second"})"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/02_msg_per_minute", util::stringToBuffer(R"({"type": "graph", "headline": "Messages per Minute"})"), QoS::QoS2, Retain::Yes});
    refresh();
}
void Statistics::publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained, MQTTPublishPacketBuilder& packetBuilder) {
    PacketData packet{topic, payload.size(), std::chrono::system_clock::now(), qos};
    push(mCollectedData, std::move(packet));
    mTotalPacketCountCounter++;
}
void Statistics::push(atomic_queue::AtomicQueueB2<PacketData>& queue, PacketData&& packet) {
    while(!queue.try_push(std::move(packet))) {
        PacketData poppedPacket;
        // pop 10 packets at once to increase the success rate of pushing
        for(int i = 0; i < 10; ++i) {
            if(!queue.try_pop(poppedPacket))
                break;
        }
    }
}
void Statistics::refreshInternal() {
    util::Stopwatch stopwatch{"Statistical analysis"};
    {
        PacketData packet;
        while(mCollectedData.try_pop(packet)) {
            mAnalysisData.emplace_back(std::move(packet));
        }
    }

    mAnalysisResult.sleepLevelSampleCounts = mSleepLevelSampleCounts;
    mAnalysisResult.appStateQueueDepth = mApp.getCurrentWorkerThreadQueueDepth();
    mAnalysisResult.currentSleepLevel = mApp.getCurrentWorkerThreadSleepLevel();
    mAnalysisResult.totalPacketCount = mTotalPacketCountCounter;
    mAnalysisResult.retainedMsgCount = mApp.getRetainedMsgCount();
    mAnalysisResult.retainedMsgCummulativeSize = mApp.getRetainedMsgCummulativeSize();
    mAnalysisResult.activeSubscriptions = mApp.getSubscriptionsCount();
    mAnalysisResult.uptimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - mStartTime).count();

    mAnalysisResult.clients.clear();
    mApp.forEachClient([&](const std::string& clientId, const std::string& hostname = {}, uint16_t port = 0) {
       mAnalysisResult.clients.emplace_back(AnalysisResults::ClientInfo{clientId, hostname, port});
    });

    for(auto& packet: mAnalysisData) {
        auto it = mAnalysisResult.topics.find(packet.topic);
        if(it == mAnalysisResult.topics.end()) {
            auto inserted = mAnalysisResult.topics.emplace(std::piecewise_construct, std::make_tuple(std::move(packet.topic)), std::make_tuple());
            assert(inserted.second);
            it = inserted.first;
        }
        auto nowRounded = std::chrono::floor<std::chrono::minutes>(std::chrono::system_clock::now());
        ensureEnoughSpace<std::chrono::minutes, 60>(it->second, nowRounded);
        it->second.back().cummulativePacketSize += packet.payloadLength;
        it->second.back().packetCount += 1;
        it->second.back().qosPacketCounts[static_cast<uint8_t>(packet.qos)] += 1;
    }
    for(auto it = mAnalysisResult.topics.begin(); it != mAnalysisResult.topics.end();) {
        if(!it->second.empty() && (it->second.begin()->timestamp + std::chrono::minutes(65)) < std::chrono::system_clock::now()) {
            it = mAnalysisResult.topics.erase(it);
        } else {
            it++;
        }
    }

    createHistogram<std::chrono::minutes, 60 * 24>(mAnalysisResult.packetsPerMinute);
    ensureEnoughSpace<std::chrono::minutes, 60 * 24>(mAnalysisResult.packetsPerMinute, std::chrono::floor<std::chrono::minutes>(std::chrono::system_clock::now()) - std::chrono::minutes(1));
    createHistogram<std::chrono::seconds, 60 * 2>(mAnalysisResult.packetsPerSecond);
    ensureEnoughSpace<std::chrono::seconds , 60 * 2>(mAnalysisResult.packetsPerSecond, std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) - std::chrono::seconds(1));

    mAnalysisData.clear();

    mApp.publishAsync(AsyncPublishData{"$NIOEV/stats", util::stringToBuffer(StatisticsConverter::statsToJson(mAnalysisResult)), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/01_msg_per_second/data", util::stringToBuffer(StatisticsConverter::statsToMsgPerSecondJsonWebUI(mAnalysisResult)), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/02_msg_per_minute/data", util::stringToBuffer(StatisticsConverter::statsToMsgPerMinuteJsonWebUI(mAnalysisResult)), QoS::QoS2, Retain::Yes});
}
AnalysisResults Statistics::getResults() {
    std::unique_lock<std::shared_mutex> lock{mMutex};
    refreshInternal();
    return mAnalysisResult;
}
void Statistics::refresh() {
    std::unique_lock<std::shared_mutex> lock{mMutex};
    refreshInternal();
}

}