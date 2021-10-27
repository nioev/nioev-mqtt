cmake_minimum_required(VERSION 3.0)
project(nioev)

set(CMAKE_CXX_STANDARD 20)
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Og")

set(SPDLOG_NO_PTHREAD_ID OFF)
add_subdirectory(thirdparty/spdlog)
add_executable(nioev
        src/main.cpp
        src/TcpServer.cpp
        src/TcpServer.hpp
        src/Util.hpp
        src/TcpClientHandlerInterface.hpp
        src/MQTTClientConnectionManager.cpp
        src/MQTTClientConnectionManager.hpp
        src/MQTTClientConnection.cpp
        src/MQTTClientConnection.hpp
        src/TcpClientConnection.cpp
        src/TcpClientConnection.hpp src/ReceiverThreadManager.cpp src/ReceiverThreadManager.hpp src/ReceiverThreadManagerExternalBridgeInterface.hpp src/Forward.hpp src/Enums.hpp src/SenderThreadManager.cpp src/SenderThreadManager.hpp src/SenderThreadManagerExternalBridgeInterface.hpp src/SubscriptionsManager.cpp src/SubscriptionsManager.hpp)
target_link_libraries(nioev spdlog pthread dl)
