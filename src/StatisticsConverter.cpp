#include "StatisticsConverter.hpp"
#include "rapidjson/document.h"
#include "rapidjson/writer.h"

namespace nioev::StatisticsConverter {

static std::string stringify(const rapidjson::Document& doc) {

    rapidjson::StringBuffer docStringified;
    rapidjson::Writer<rapidjson::StringBuffer> docWriter{ docStringified };
    doc.Accept(docWriter);
    std::string body{ docStringified.GetString(), docStringified.GetLength() };
    return body;
}

std::string nioev::StatisticsConverter::statsToJson(const nioev::AnalysisResults& stats) {
    rapidjson::Document doc;
    doc.SetObject();

    doc.AddMember(rapidjson::StringRef("total_msg_count"), rapidjson::Value{ stats.totalPacketCount }, doc.GetAllocator());
    doc.AddMember(rapidjson::StringRef("current_sleep_level"), rapidjson::StringRef(workerThreadSleepLevelToString(stats.currentSleepLevel)), doc.GetAllocator());
    doc.AddMember(rapidjson::StringRef("app_state_queue_depth"), rapidjson::Value{ stats.appStateQueueDepth }, doc.GetAllocator());
    doc.AddMember(rapidjson::StringRef("retained_msg_count"), rapidjson::Value{ stats.retainedMsgCount }, doc.GetAllocator());
    doc.AddMember(rapidjson::StringRef("retained_msg_size_sum"), rapidjson::Value{ stats.retainedMsgCummulativeSize }, doc.GetAllocator());
    doc.AddMember(rapidjson::StringRef("uptime_seconds"), rapidjson::Value{ stats.uptimeSeconds }, doc.GetAllocator());
    {
        rapidjson::Value subs;
        subs.SetObject();
        for(auto& sub : stats.activeSubscriptions) {
            subs.AddMember(
                rapidjson::Value{ sub.first.c_str(), static_cast<rapidjson::SizeType>(sub.first.size()), doc.GetAllocator() }, rapidjson::Value{ sub.second }, doc.GetAllocator());
        }
        doc.AddMember(rapidjson::StringRef("active_subscriptions"), std::move(subs.Move()), doc.GetAllocator());
    }
    {
        rapidjson::Value clients;
        clients.SetObject();
        for(auto& c : stats.clients) {
            rapidjson::Value val;
            val.SetObject();
            val.AddMember(rapidjson::StringRef("hostname"), rapidjson::Value{ c.hostname.c_str(), static_cast<rapidjson::SizeType>(c.hostname.size()), doc.GetAllocator() }, doc.GetAllocator());
            val.AddMember(rapidjson::StringRef("port"), c.port, doc.GetAllocator());
            clients.AddMember(
                rapidjson::Value{ c.clientId.c_str(), static_cast<rapidjson::SizeType>(c.clientId.size()), doc.GetAllocator() }, std::move(val.Move()), doc.GetAllocator());
        }
        doc.AddMember(rapidjson::StringRef("clients"), std::move(clients.Move()), doc.GetAllocator());
    }
    {
        rapidjson::Value sleepLevelCounts;
        sleepLevelCounts.SetObject();
        for(auto& t : stats.sleepLevelSampleCounts) {
            if(&t == &stats.sleepLevelSampleCounts.back()) {
                continue;
            }
            rapidjson::Value tobj;
            tobj.SetObject();
            for(int i = 0; i < static_cast<int>(WorkerThreadSleepLevel::$COUNT); ++i) {
                tobj.AddMember(rapidjson::StringRef(workerThreadSleepLevelToString(static_cast<WorkerThreadSleepLevel>(i))), rapidjson::Value{ t.samples.at(i) }, doc.GetAllocator());
            }
            auto str = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(t.timestamp.time_since_epoch()).count());
            sleepLevelCounts.AddMember(rapidjson::Value{ str.c_str(), static_cast<rapidjson::SizeType>(str.size()), doc.GetAllocator() }, std::move(tobj.Move()), doc.GetAllocator());
        }
        doc.AddMember(rapidjson::StringRef("sleep_level_sample_counts"), std::move(sleepLevelCounts.Move()), doc.GetAllocator());
    }
    {
        rapidjson::Value topics;
        topics.SetObject();
        for(auto& topic : stats.topics) {
            rapidjson::Value topicJson;
            topicJson.SetObject();
            for(auto& ti : topic.second) {
                if(&ti == &topic.second.back())
                    continue;
                rapidjson::Value tiJson;
                tiJson.SetObject();
                tiJson.AddMember(rapidjson::StringRef("msg_size_sum"), rapidjson::Value{ ti.cummulativePacketSize }, doc.GetAllocator());
                tiJson.AddMember(rapidjson::StringRef("msg_count"), rapidjson::Value{ ti.packetCount }, doc.GetAllocator());
                tiJson.AddMember(rapidjson::StringRef("qos_0_packet_count"), rapidjson::Value{ ti.qosPacketCounts.at(0) }, doc.GetAllocator());
                tiJson.AddMember(rapidjson::StringRef("qos_1_packet_count"), rapidjson::Value{ ti.qosPacketCounts.at(1) }, doc.GetAllocator());
                tiJson.AddMember(rapidjson::StringRef("qos_2_packet_count"), rapidjson::Value{ ti.qosPacketCounts.at(2) }, doc.GetAllocator());

                auto str = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(ti.timestamp.time_since_epoch()).count());
                topicJson.AddMember(rapidjson::Value{ str.c_str(), static_cast<rapidjson::SizeType>(str.size()), doc.GetAllocator() }, std::move(tiJson.Move()), doc.GetAllocator());
            }
            topics.AddMember(
                rapidjson::Value{ topic.first.c_str(), static_cast<rapidjson::SizeType>(topic.first.size()), doc.GetAllocator() }, std::move(topicJson.Move()), doc.GetAllocator());
        }
        doc.AddMember(rapidjson::StringRef("topics"), std::move(topics.Move()), doc.GetAllocator());
    }
    {
        auto addHistogram = [&](const std::vector<AnalysisResults::TimeInfo>& data, const char* name) {
            rapidjson::Value obj;
            obj.SetObject();
            for(auto& interval: data) {
                if(&interval == &data.back())
                    continue;
                rapidjson::Value intervalObj;
                intervalObj.SetObject();
                intervalObj.AddMember(rapidjson::StringRef("msg_count"), rapidjson::Value{ interval.packetCount }, doc.GetAllocator());
                intervalObj.AddMember(rapidjson::StringRef("msg_size_sum"), rapidjson::Value{ interval.cummulativePacketSize }, doc.GetAllocator());
                auto str = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(interval.timestamp.time_since_epoch()).count());
                obj.AddMember(rapidjson::Value{ str.c_str(), static_cast<rapidjson::SizeType>(str.size()), doc.GetAllocator() }, std::move(intervalObj.Move()), doc.GetAllocator());
            }
            doc.AddMember(rapidjson::StringRef(name), std::move(obj.Move()), doc.GetAllocator());
        };
        addHistogram(stats.packetsPerSecond, "msg_per_second");
        addHistogram(stats.packetsPerMinute, "msg_per_minute");
    }
    return stringify(doc);
}
std::string statsToMsgPerSecondJsonWebUI(const AnalysisResults& res) {
    rapidjson::Document doc;
    doc.SetArray();
    for(auto& m: res.packetsPerSecond) {
        rapidjson::Value obj;
        obj.SetObject();
        obj.AddMember(rapidjson::StringRef("x"), rapidjson::Value{std::chrono::duration_cast<std::chrono::seconds>(m.timestamp.time_since_epoch()).count()}, doc.GetAllocator());
        obj.AddMember(rapidjson::StringRef("y"), rapidjson::Value{m.packetCount}, doc.GetAllocator());
        doc.PushBack(std::move(obj.Move()), doc.GetAllocator());
    }

    return stringify(doc);
}
std::string statsToMsgPerMinuteJsonWebUI(const AnalysisResults& res) {
    rapidjson::Document doc;
    doc.SetArray();
    for(auto& m: res.packetsPerMinute) {
        rapidjson::Value obj;
        obj.SetObject();
        obj.AddMember(rapidjson::StringRef("x"), rapidjson::Value{std::chrono::duration_cast<std::chrono::seconds>(m.timestamp.time_since_epoch()).count()}, doc.GetAllocator());
        obj.AddMember(rapidjson::StringRef("y"), rapidjson::Value{m.packetCount}, doc.GetAllocator());
        doc.PushBack(std::move(obj.Move()), doc.GetAllocator());
    }
    return stringify(doc);
}
std::string stringToJSON(const std::string& str) {
    rapidjson::Document doc;
    doc.SetString(str.c_str(), str.size());
    return stringify(doc);
}
std::string stringListToJSON(const std::vector<std::string>& strs) {
    rapidjson::Document doc;
    doc.SetArray();
    for(auto& s: strs) {
        doc.PushBack(rapidjson::StringRef(s.c_str(), s.size()), doc.GetAllocator());
    }
    return stringify(doc);
}

}