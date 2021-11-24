cmake_minimum_required(VERSION 3.0)
project(uWebSockets)

add_definitions(-DCONFIG_VERSION="2021-03-27" -DCONFIG_BIGNUM -D_GNU_SOURCE)

add_definitions(-DLIBUS_NO_SSL)
file(GLOB_RECURSE FILES "../uWebSockets/uSockets/src/*.c")

include_directories("../uWebSockets/uSockets/src")
add_library(uWebSockets STATIC ${FILES})
target_link_libraries(uWebSockets z)