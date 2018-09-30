#!/bin/bash
# Reference: https://stackoverflow.com/a/1289907
g++ -O3 -g -fno-stack-protector -c Main.cc -lpthread 2>/dev/null
objdump -drwC -Mintel --no-show-raw-insn Main.o > Main.s
awk '/run_.*:/' RS= Main.s > run_loops_assembly.txt
rm Main.o
echo "Output written to Main.s and run_loops_assembly.txt"
