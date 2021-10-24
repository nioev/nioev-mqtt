#pragma once

#include <string>
#include <cstring>

#include "spdlog/spdlog.h"

namespace nioev {

using uint = unsigned int;

namespace util {

inline std::string errnoToString() {
    char buffer[1024] = { 0 };
    return strerror_r(errno, buffer, 1024);
}

inline void throwErrno(std::string msg) {
    throw std::runtime_error{msg + ": " + errnoToString()};
}
}
}