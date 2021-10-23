#!/bin/bash
find mqtt-client mqtt-client-cli -name *.hpp -o -name *.cpp |xargs clang-format -i -style=file
