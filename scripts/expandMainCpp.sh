#!/bin/bash
echo "#include <cstdint>" > main.cpp
echo "#include <cstdio>" >> main.cpp
echo "#include <thread>" >> main.cpp
g++ -E -nostdinc Main.cc 2>/dev/null | grep -v '^# ' >> main.cpp
clang-format -i main.cpp
