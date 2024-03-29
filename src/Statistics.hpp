#pragma once

#include "Subscriber.hpp"
#include "nioev/lib/Timers.hpp"
#include "atomic_queue/atomic_queue.h"
#include <shared_mutex>

namespace nioev::mqtt {

struct PacketData {
    std::string topic;
    size_t payloadLength{0};
    std::chrono::system_clock::time_point timestamp;
    QoS qos{QoS::QoS0};
};

struct SleepLevelSampleCounts {
    std::array<uint64_t, static_cast<size_t>(WorkerThreadSleepLevel::$COUNT)> samples;
    std::chrono::system_clock::time_point timestamp;
    SleepLevelSampleCounts() {
        samples.fill(0);
    }
};

struct AnalysisResults {
    struct TopicInfo {
        uint64_t cummulativePacketSize{0};
        uint64_t packetCount{0};
        std::array<uint64_t, 3> qosPacketCounts{0, 0, 0};
        std::chrono::system_clock::time_point timestamp;
    };
    std::unordered_map<std::string, std::vector<TopicInfo>> topics;
    uint64_t totalPacketCount{0};
    uint64_t appStateQueueDepth{0};
    uint64_t retainedMsgCount{0};
    uint64_t retainedMsgCummulativeSize{0};
    uint64_t uptimeSeconds{0};

    std::unordered_map<std::string, uint64_t> activeSubscriptions;

    struct TimeInfo {
        std::chrono::system_clock::time_point timestamp;
        uint64_t packetCount{0};
        uint64_t cummulativePacketSize{0};
    };
    std::vector<TimeInfo> packetsPerMinute;
    std::vector<TimeInfo> packetsPerSecond;

    struct ClientInfo {
        std::string clientId;
        std::string hostname;
        uint16_t port{0};
    };
    std::vector<ClientInfo> clients;

    WorkerThreadSleepLevel currentSleepLevel{WorkerThreadSleepLevel::YIELD};
    std::vector<SleepLevelSampleCounts> sleepLevelSampleCounts{};
};
/* This class is kind of similiar to the kappa architecture.
 */
class Statistics : public Subscriber {
public:
    Statistics(ApplicationState& app);
    void init();
    void publish(const std::string& topic, PayloadType payload, QoS qos, Retained retained, const PropertyList& properties, MQTTPublishPacketBuilder& packetBuilder) override;
    AnalysisResults getResults();
    virtual const char* getType() const override {
        return "stats";
    }
    void refresh();
private:
    void push(atomic_queue::AtomicQueueB2<PacketData>& queue, PacketData&& packet);
    void refreshInternal();

    template<typename Interval, size_t MaxSize>
    void createHistogram(std::vector<AnalysisResults::TimeInfo>& list) {
        for(auto it = mAnalysisData.begin(); it != mAnalysisData.end(); ++it) {
            auto rounded = std::chrono::floor<Interval>(it->timestamp);
            ensureEnoughSpace<Interval, MaxSize>(list, rounded);
            list.back().packetCount += 1;
            list.back().cummulativePacketSize += it->payloadLength;
        }
    };

    template<typename Interval, size_t MaxSize, typename T>
    void ensureEnoughSpace(std::vector<T>& list, std::chrono::system_clock::time_point roundedTimestamp) {
        if(list.empty()) {
            list.emplace_back();
            list.back().timestamp = roundedTimestamp;
        } else {
            while(list.back().timestamp < roundedTimestamp) {
                list.emplace_back();
                list.back().timestamp = list.at(list.size() - 2).timestamp + Interval{1};

                while(list.size() > MaxSize) {
                    list.erase(list.begin());
                }
            }
        }
        while(list.size() > MaxSize) {
            list.erase(list.begin());
        }
    }

    atomic_queue::AtomicQueueB2<PacketData> mCollectedData{100'000};
    std::atomic<uint64_t> mTotalPacketCountCounter{0};
    std::vector<PacketData> mAnalysisData;
    ApplicationState& mApp;

    std::shared_mutex mMutex;
    std::vector<SleepLevelSampleCounts> mSleepLevelSampleCounts;
    AnalysisResults mAnalysisResult;
    std::chrono::steady_clock::time_point mStartTime;
    Timers mBatchAnalysisTimer, mSampleWorkerThreadTimer; // TODO make single timer
};

}