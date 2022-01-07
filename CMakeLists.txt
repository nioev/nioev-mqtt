cmake_minimum_required(VERSION 3.0)
project(nioev)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")

set(CMAKE_EXE_LINKER_FLAGS "-Wl,--export-dynamic")

set(SPDLOG_NO_PTHREAD_ID OFF)
add_subdirectory(thirdparty/spdlog)
add_subdirectory(thirdparty/quickjs-2021-03-27)
include_directories(thirdparty/quickjs-2021-03-27)
include_directories(thirdparty/zstd/lib)

include_directories("thirdparty/uWebSockets/uSockets/src")
include_directories(thirdparty/uWebSockets/src)
include_directories(thirdparty/atomic_queue/include)
include_directories(thirdparty/rapidjson/include)
add_subdirectory(thirdparty/uWebSockets-CMake)
add_subdirectory(thirdparty/zstd/build/cmake)
add_subdirectory(thirdparty/SQLiteCpp)

include_directories (${CMAKE_BINARY_DIR})
add_custom_command(OUTPUT quickjs-carr.h COMMAND bash ${PROJECT_SOURCE_DIR}/package_quick.sh ${CMAKE_BINARY_DIR} SOURCES ${PROJECT_SOURCE_DIR}/thirdparty/quickjs-2021-03-27/quickjs.h)

add_executable(nioev
        src/main.cpp
        src/TcpServer.cpp
        src/TcpServer.hpp
        src/Util.hpp
        src/TcpClientHandlerInterface.hpp
        src/MQTTClientConnection.hpp
        src/TcpClientConnection.cpp
        src/TcpClientConnection.hpp
        src/ClientThreadManager.cpp
        src/ClientThreadManager.hpp
        src/Forward.hpp
        src/Enums.hpp
        src/scripting/ScriptContainerManager.hpp
        src/scripting/ScriptContainer.hpp
        src/scripting/ScriptContainerJS.cpp
        src/scripting/ScriptContainerJS.hpp
        src/BigString.hpp
        src/BigVector.hpp
        ${UWEBSOCKETS_FILES}
        src/Timers.cpp
        src/Timers.hpp
        src/scripting/ScriptCustomTCPServer.cpp
        src/scripting/ScriptCustomTCPServer.hpp
        src/scripting/ScriptContainerManager.cpp
        src/ApplicationState.cpp
        src/ApplicationState.hpp
        src/Subscriber.hpp
        src/AsyncPublisher.cpp
        src/AsyncPublisher.hpp
        src/MQTTClientConnection.cpp
        src/Util.cpp
        src/MQTTPublishPacketBuilder.hpp
        src/MQTTPublishPacketBuilder.cpp
        src/scripting/NativeLibrary.cpp
        src/scripting/NativeLibrary.hpp
        src/scripting/NativeLibraryManager.cpp
        src/scripting/NativeLibraryManager.hpp
        src/GenServer.hpp
        quickjs-carr.h
        src/quickjs_h_embedded.hpp
        src/quickjs_h_embedded.cpp)

target_link_libraries(nioev
        libzstd_static
        uWebSockets
        spdlog
        quickjs
        SQLiteCpp
        sqlite3
        pthread
        dl
        m
        stdc++)
