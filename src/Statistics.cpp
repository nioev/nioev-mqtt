#include "Statistics.hpp"
#include "ApplicationState.hpp"
#include "MQTTPublishPacketBuilder.hpp"
#include "StatisticsConverter.hpp"
#include "Timers.hpp"

namespace nioev::mqtt {

Statistics::Statistics(ApplicationState& app)
: mApp(app) {
    mBatchAnalysisTimer.addPeriodicTask(std::chrono::seconds(1), [this] {
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
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt", stringToBuffer("{}"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats", stringToBuffer(R"({"type": "list", "orientation": "vertical"})"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/00_toolbar", stringToBuffer(R"({"type": "grid", "stretch": true, "columnWidth": "300px"})"), QoS::QoS2, Retain::Yes});
    //mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/00_toolbar/99_pinger", stringToBuffer(R"({"type": "pinger", "topic": "$NIOEV/request_new_stats", "interval_ms": 15000})"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/05_grid", stringToBuffer(R"({"type": "grid"})"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/05_grid/01_msg_per_second", stringToBuffer(R"({"type": "graph", "headline": "Messages per Second"})"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/05_grid/02_msg_per_minute", stringToBuffer(R"({"type": "graph", "headline": "Messages per Minute"})"), QoS::QoS2, Retain::Yes});
    refresh();
}
void Statistics::publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained, MQTTPublishPacketBuilder& packetBuilder) {
    // TODO remove system clock syscall somehow?
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
    Stopwatch stopwatch{"Statistical analysis"};
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

    mApp.publishAsync(AsyncPublishData{"$NIOEV/stats", stringToBuffer(StatisticsConverter::statsToJson(mAnalysisResult)), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/05_grid/01_msg_per_second/data", stringToBuffer(StatisticsConverter::statsToMsgPerSecondJsonWebUI(mAnalysisResult)), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/05_grid/02_msg_per_minute/data", stringToBuffer(StatisticsConverter::statsToMsgPerMinuteJsonWebUI(mAnalysisResult)), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/00_toolbar/00_version", stringToBuffer(R"({"type": "twoline", "content": "Alpha", "headline": "Version"})"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/00_toolbar/05_uptime",
                                        stringToBuffer(R"({"type": "twoline", "content": )" + std::to_string(mAnalysisResult.uptimeSeconds) +  R"(, "headline": "Uptime"})"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/00_toolbar/10_total_msg",
                                        stringToBuffer(R"({"type": "twoline", "content": )" + std::to_string(mAnalysisResult.totalPacketCount) +  R"(, "headline": "Total Messages"})"), QoS::QoS2, Retain::Yes});

    size_t subs = 0;
    for(auto& s: mAnalysisResult.activeSubscriptions) {
        subs += s.second;
    }
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/00_toolbar/15_active_subs",
                                        stringToBuffer(R"({"type": "twoline", "content": )" + std::to_string(subs) +  R"(, "headline": "Active Subscriptions"})"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/00_toolbar/20_sleep_state",
                                        stringToBuffer(R"({"type": "twoline", "content": ")" + std::string(workerThreadSleepLevelToString(mAnalysisResult.currentSleepLevel)) +  R"(", "headline": "Sleep State"})"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/00_toolbar/25_app_queue_depth",
                                        stringToBuffer(R"({"type": "twoline", "content": )" + std::to_string(mAnalysisResult.appStateQueueDepth) +  R"(, "headline": "App Queue Depth"})"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/00_toolbar/30_retained_count",
                                        stringToBuffer(R"({"type": "twoline", "content": )" + std::to_string(mAnalysisResult.retainedMsgCount) +  R"(, "headline": "Retained Count"})"), QoS::QoS2, Retain::Yes});
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/00_toolbar/35_retained_bytes",
                                        stringToBuffer(R"({"type": "twoline", "content": )" + std::to_string(mAnalysisResult.retainedMsgCummulativeSize) +  R"(, "headline": "Retained Bytes"})"), QoS::QoS2, Retain::Yes});
    std::vector<std::string> rows;
    for(auto& c: mAnalysisResult.clients) {
        std::string row;
        row += c.clientId;
        row += " connected from: ";
        row += c.hostname + ":" + std::to_string(c.port);
        rows.emplace_back(std::move(row));
    }
    mApp.publishAsync(AsyncPublishData{"nioev/ui/services/mqtt/Stats/05_grid/05_clients", stringToBuffer(R"({"type": "items", "headline": "Clients", "lines": )" + StatisticsConverter::stringListToJSON(rows) + "}"), QoS::QoS2, Retain::Yes});
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