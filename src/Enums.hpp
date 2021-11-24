#pragma once

#include <cstdint>

namespace nioev {

enum class MQTTMessageType : uint8_t
{
    Invalid = 0,
    CONNECT = 1,
    CONNACK = 2,
    PUBLISH = 3,
    PUBACK = 4,
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

}