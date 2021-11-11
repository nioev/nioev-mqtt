cmake_minimum_required(VERSION 3.0)
project(quickjs)

add_definitions(-DCONFIG_VERSION="2021-03-27" -DCONFIG_BIGNUM -D_GNU_SOURCE)
file(GLOB FILES ${PROJECT_SOURCE_DIR}/*.c ${PROJECT_SOURCE_DIR}/*.h)
add_library(quickjs STATIC ${FILES})
