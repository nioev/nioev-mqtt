#pragma once

#include "Subscriber.hpp"
#include "Timers.hpp"
#include "atomic_queue/atomic_queue.h"
#include <shared_mutex>

namespace nioev {

struct PacketData {
    std::string topic;
    size_t payloadLength{0};
    std::chrono::steady_clock::time_point timestamp;
    QoS qos{QoS::QoS0};
};

struct AnalysisResults {
    struct TopicInfo {
        uint64_t cummulativePacketSize{0};
        uint64_t packetCount{0};
        std::array<uint64_t, 3> qosPacketCounts{0, 0, 0};
    };
    std::unordered_map<std::string, TopicInfo> topics;
    uint64_t totalPacketCount{0};
    uint64_t appStateQueueDepth{0};
    uint64_t retainedMsgCount{0};
    uint64_t retainedMsgCummulativeSize{0};

    struct TimeInfo {
        std::chrono::steady_clock::time_point timestamp;
        uint64_t packetCount{0};
    };
    std::vector<TimeInfo> packetsPerMinute;
    std::vector<TimeInfo> packetsPerSecond;

    WorkerThreadSleepLevel currentSleepLevel{WorkerThreadSleepLevel::YIELD};
    std::array<uint64_t, static_cast<size_t>(WorkerThreadSleepLevel::$COUNT)> sleepLevelSampleCounts{};
    AnalysisResults() {
        sleepLevelSampleCounts.fill(0);
    }
};
/* This class is kind of similiar to the kappa architecture.
 */
class Statistics : public Subscriber {
public:
    Statistics(ApplicationState& app);
    void init();
    void publish(const std::string& topic, const std::vector<uint8_t>& payload, QoS qos, Retained retained, MQTTPublishPacketBuilder& packetBuilder) override;
    AnalysisResults getResults();
private:
    void push(atomic_queue::AtomicQueueB2<PacketData>& queue, PacketData&& packet);
    void refresh();

    template<typename Interval, size_t MaxSize>
    void createHistogram(std::vector<AnalysisResults::TimeInfo>& list) {
        if(list.size() > MaxSize) {
            list.erase(list.begin() + MaxSize, list.end());
        }
        for(auto it = mAnalysisData.rbegin(); it != mAnalysisData.rend(); ++it) {
            auto rounded = std::chrono::round<Interval>(it->timestamp);
            if(list.empty() || list.back().timestamp != rounded) {
                if(list.size() > MaxSize) {
                    break;
                }
                list.emplace_back(AnalysisResults::TimeInfo{rounded, 1});
            } else {
                list.back().packetCount += 1;
            }
        }
    };

    Timers mBatchAnalysisTimer, mSampleWorkerThreadTimer;
    atomic_queue::AtomicQueueB2<PacketData> mCollectedData{100'000};
    std::atomic<uint64_t> mTotalPacketCountCounter{0};
    std::vector<PacketData> mAnalysisData;
    ApplicationState& mApp;

    std::shared_mutex mMutex;
    std::array<uint64_t, static_cast<size_t>(WorkerThreadSleepLevel::$COUNT)> mSleepLevelSampleCounts;
    AnalysisResults mAnalysisResult;
};

}