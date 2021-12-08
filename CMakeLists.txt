cmake_minimum_required(VERSION 3.0)
project(nioev)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")

set(SPDLOG_NO_PTHREAD_ID OFF)
add_subdirectory(thirdparty/spdlog)
add_subdirectory(thirdparty/quickjs-2021-03-27)
include_directories(thirdparty/quickjs-2021-03-27)
include_directories(thirdparty/zstd/lib)

include_directories("thirdparty/uWebSockets/uSockets/src")
include_directories(thirdparty/uWebSockets/src)
add_subdirectory(thirdparty/uWebSockets-CMake)
add_subdirectory(thirdparty/zstd/build/cmake)

add_executable(nioev
        src/main.cpp
        src/TcpServer.cpp
        src/TcpServer.hpp
        src/Util.hpp
        src/TcpClientHandlerInterface.hpp
        src/Application.cpp
        src/Application.hpp
        src/MQTTClientConnection.cpp
        src/MQTTClientConnection.hpp
        src/TcpClientConnection.cpp
        src/TcpClientConnection.hpp
        src/ClientThreadManager.cpp
        src/ClientThreadManager.hpp
        src/Forward.hpp
        src/Enums.hpp
        src/MQTTPersistentState.cpp
        src/MQTTPersistentState.hpp
        src/scripting/ScriptContainerManager.hpp
        src/scripting/ScriptContainer.hpp
        src/scripting/ScriptContainerJS.cpp
        src/scripting/ScriptContainerJS.hpp
        src/BigString.hpp
        src/BigVector.hpp
        src/scripting/ScriptActionPerformer.cpp
        src/scripting/ScriptActionPerformer.hpp
        ${UWEBSOCKETS_FILES} src/Timers.cpp src/Timers.hpp src/scripting/ScriptCustomTCPServer.cpp src/scripting/ScriptCustomTCPServer.hpp src/scripting/ScriptContainerManager.cpp)

target_link_libraries(nioev libzstd_static uWebSockets spdlog quickjs pthread dl)
