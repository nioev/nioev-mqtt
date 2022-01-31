#pragma once

#include <cassert>
#include <cstdint>

namespace nioev {

enum class MQTTMessageType : uint8_t
{
    Invalid = 0,
    CONNECT = 1,
    CONNACK = 2,
    PUBLISH = 3,
    PUBACK = 4,
    PUBREC = 5,
    PUBREL = 6,
    PUBCOMP = 7,
    SUBSCRIBE = 8,
    SUBACK = 9,
    UNSUBSCRIBE = 10,
    UNSUBACK = 11,
    PINGREQ = 12,
    PINGRESP = 13,
    DISCONNECT = 14,
    Count = 15
};

enum class QoS : uint8_t
{
    QoS0 = 0,
    QoS1 = 1,
    QoS2 = 2,
};

static inline QoS minQoS(QoS a, QoS b) {
    return static_cast<uint8_t>(a) < static_cast<uint8_t>(b) ? a : b;
}

enum class Retained
{
    No,
    Yes
};
using Retain = Retained;

enum class CleanSession
{
    Yes,
    No
};

enum class Compression
{
    NONE,
    ZSTD
};

enum class WorkerThreadSleepLevel : int
{
    YIELD,
    MICROSECONDS,
    MILLISECONDS,
    TENS_OF_MILLISECONDS,
    $COUNT
};

static inline const char* workerThreadSleepLevelToString(WorkerThreadSleepLevel level) {
    switch(level) {
    case WorkerThreadSleepLevel::YIELD:
        return "yield";
    case WorkerThreadSleepLevel::MICROSECONDS:
        return "microseconds";
    case WorkerThreadSleepLevel::MILLISECONDS:
        return "milliseconds";
    case WorkerThreadSleepLevel::TENS_OF_MILLISECONDS:
        return "tens_of_milliseconds";
    }
    assert(false);
    return "";
}

}