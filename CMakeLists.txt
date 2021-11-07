cmake_minimum_required(VERSION 3.0)
project(nioev)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")

set(SPDLOG_NO_PTHREAD_ID OFF)
add_subdirectory(thirdparty/spdlog)
add_executable(nioev
        src/mqtt/main.cpp
        src/mqtt/TcpServer.cpp
        src/mqtt/TcpServer.hpp
        src/mqtt/Util.hpp
        src/mqtt/TcpClientHandlerInterface.hpp
        src/mqtt/MQTTClientConnectionManager.cpp
        src/mqtt/MQTTClientConnectionManager.hpp
        src/mqtt/MQTTClientConnection.cpp
        src/mqtt/MQTTClientConnection.hpp
        src/mqtt/TcpClientConnection.cpp
        src/mqtt/TcpClientConnection.hpp src/mqtt/ClientThreadManager.cpp src/mqtt/ClientThreadManager.hpp src/mqtt/ClientThreadManagerExternalBridgeInterface.hpp src/mqtt/Forward.hpp src/mqtt/Enums.hpp src/mqtt/SubscriptionsManager.cpp src/mqtt/SubscriptionsManager.hpp src/scripting/ScriptManager.cpp src/scripting/ScriptManager.hpp)
target_link_libraries(nioev spdlog pthread dl)
