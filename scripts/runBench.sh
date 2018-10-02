#!/bin/bash
for i in {1..5};
do
	perf stat -r 3 ./FastLog 1 10000000 $i
done