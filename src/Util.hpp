#pragma once

#include <string>
#include <cstring>

#include "spdlog/spdlog.h"

namespace nioev::util {

std::string errnoToString() {
    char buffer[1024] = { 0 };
    strerror_r(errno, buffer, 1024);
    return buffer;
}
}