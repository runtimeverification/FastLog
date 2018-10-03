#!/bin/bash
for i in {1..10};
do
	sudo cset shield --exec -- perf stat -r 5 ./FastLog 1 10000000 $i
done