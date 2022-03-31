nioev-mqtt is a modern multithreaded mqtt broker with a focus on performance and flexibility.

## Features

 - Full support for MQTT 3.1.1
 - Built-in JS scripting engine based on QuickJS
 - WebUI for viewing statistics and configuring scripts
 - REST API & WS API for simple publishing & subscribing
 - Written in C++

## Performance

nioev-mqtt is heavily focused on performance. To my knowledge and limited, primitive testing, it's the fastest
mqtt broker out there. It is written in C++ and heavily multithreaded, as opposed to [mosquitto](https://mosquitto.org/),
which only uses a single thread. In all my tests, nioev-mqtt can handle up to 2 times more packets per second if you've got 
at least 4 cores available. In some very synthetic benchmarks it was even 5 times faster. 
Other proprietary MQTT brokers like [VerneMQ](https://vernemq.com/) or [HiveMQ](https://www.hivemq.com/) are even 
slower than [mosquitto](https://mosquitto.org/) in most cirumstances.

Of course this is hard to notice, as under most circumstances, network is the bottleneck, not the CPU. 
This is mostly only useful under very complex loads with many subscribers or when you've got some very low-power 
hardware like Raspberry Pis. 

TODO add some nice benchmark graphs.

## Flexibility

nioev-mqtt utilizes QuickJS to allow scripts to run directly within the broker. Each script runs a separate thread and
can subscribe & publish. You can also write libraries in C++, which are compiled as dynamic libraries on the 
server and can then be loaded from within scripts. This allows you to bind to e.g. cairo to render graphics (which is
the use case which I developed this system for).

The scripting API is missing a lot of features, which I will add when I have a use case. Being built into the broker itself
allows the scripts to interact in non-standard ways with the broker. Features I'm considering include:

 - Simple API for fetching reatined values
 - Persistent storage for saving values between restarts
 - Intercepting packets and controlling who receives them
 - Receiving information about the sender of received packets to e.g. ignore packets from other scripts

## Still missing features

Please note that nioev-mqtt isn't as fully featured as [mosquitto](https://mosquitto.org/). Features
that are missing include:

 - User authentification (should be easy, I just have no need for it and so haven't looked into it)
 - Configuration file for configuring e.g. ports, database location, maximum queue depths, timeouts etc.
 - Efficient handling of many subscriptions - we don't build a tree structure right now. Simple supscriptions (no wildcards)
   are stored in a hash map, the rest is are in a simple vector.
 - io_uring
 - MQTT 5
 - TLS
 - Bridge mode
 - Documentation beyond this file (feel free to open an issue if you need assistance)
 - Other advanced features
 - A pretty icon

## Getting started

Note: nioev-mqtt only supports linux.
```bash
git clone --recursive https://github.com/VayuDev/nioev-mqtt.git
cd nioev-mqtt
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j8
./nioev
```

The broker runs on port 1883. The WebUI is availaible at http://localhost:1884. 
An example script can be found in [examples/test.js](examples/test.js).

## Thank you to the following projects which nioev-mqtt uses:

- [atomic_queue](https://github.com/max0x7ba/atomic_queue) for storing messages for other threads
- [QuickJS](https://bellard.org/quickjs/) for the scripting engine
- [rapidjson](https://github.com/Tencent/rapidjson/) for serialization
- [spdlog](https://github.com/gabime/spdlog) for logging (though we use a custom fork)
- [SQLiteCpp](https://github.com/SRombauts/SQLiteCpp) and [SQLite](https://www.sqlite.org/index.html) for persistent
  storage of scripts & retained messages
- [uWebSockets](https://github.com/uNetworking/uWebSockets) for the WebUI and the REST & WS APIs
- [svelte](https://svelte.dev/) for the WebUI

### Honorable mentions

- [valgrind](https://valgrind.org/) for debugging (especially race condtions on ARM - 
  just why do races happen so much more often on ARM than on x86?!)
- [zstd](https://github.com/facebook/zstd) which was previously used in the scripting engine and I haven't yet removed
