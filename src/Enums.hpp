#pragma once

#include <cstdint>

namespace nioev {

enum class MQTTMessageType : uint8_t
{
    Invalid = 0,
    CONNECT = 1,
    CONNACK = 2,
    Count = 15
};

}