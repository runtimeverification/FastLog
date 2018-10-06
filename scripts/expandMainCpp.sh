#!/bin/bash
echo > main.cpp
for header in atomic cassert cstdint cstdio mutex thread vector unordered_set
do
	echo "#include <$header>" >> main.cpp
done
g++ -E -nostdinc Main.cc 2>/dev/null | grep -v '^# ' >> main.cpp
clang-format -i main.cpp
