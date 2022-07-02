#pragma once
#include <string>
#include "Statistics.hpp"

namespace nioev::StatisticsConverter {

std::string statsToJson(const AnalysisResults& res);
std::string statsToMsgPerSecondJsonWebUI(const AnalysisResults& res);
std::string statsToMsgPerMinuteJsonWebUI(const AnalysisResults& res);

}
