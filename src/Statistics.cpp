#include "Statistics.hpp"
#include "MQTTPublishPacketBuilder.hpp"
#include "Timers.hpp"
#include "ApplicationState.hpp"

namespace nioev {

Statistics::Statistics(ApplicationState& app)
: mApp(app) {
    mBatchAnalysisTimer.addPeriodicTask(std::chrono::minutes(1), [this] {
        std::unique_lock<std::shared_mutex> lock{mMutex};
        refresh();
    });
    mSampleWorkerThreadTimer.addPeriodicTask(std::chrono::milliseconds(20), [this] {
        auto currentSleepLevel = mApp.getCurrentWorkerThreadSleepLevel();
        {
            std::unique_lock<std::shared_mutex> lock{mMutex};
            auto nowRounded = std::chrono::floor<std::chrono::minutes>(std::chrono::system_clock::now());
            ensureEnoughSpace<60>(mSleepLevelSampleCounts, nowRounded);
            mSleepLevelSampleCounts.back().samples.at(static_cast<int>(currentSleepLevel)) += 1;
        }
    });
    mStartTime = std::chrono::steady_clock::now();
}
void Statistics::init() {
    mApp.requestChange(ChangeRequestSubscribe{makeShared(), "", {}, SubscriptionType::OMNI, QoS::QoS2});
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
void Statistics::refresh() {
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

    for(auto& packet: mAnalysisData) {
        auto it = mAnalysisResult.topics.find(packet.topic);
        if(it == mAnalysisResult.topics.end()) {
            auto inserted = mAnalysisResult.topics.emplace(std::piecewise_construct, std::make_tuple(std::move(packet.topic)), std::make_tuple());
            assert(inserted.second);
            it = inserted.first;
        }
        auto nowRounded = std::chrono::floor<std::chrono::minutes>(std::chrono::system_clock::now());
        ensureEnoughSpace<60>(it->second, nowRounded);
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
    createHistogram<std::chrono::seconds, 60 * 2>(mAnalysisResult.packetsPerSecond);

    mAnalysisData.clear();
}
AnalysisResults Statistics::getResults() {
    std::unique_lock<std::shared_mutex> lock{mMutex};
    refresh();
    return mAnalysisResult;
}

}