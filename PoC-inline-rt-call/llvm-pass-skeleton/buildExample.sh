#!/bin/bash
# Mimic the current compilation pipeline of RV-Predict. `logop` is not inlined.
OPT=-O3
clang $OPT -c rtlib.c
clang $OPT -Xclang -load -Xclang build/skeleton/libSkeletonPass.so -c example.c
clang $OPT example.o rtlib.o -o example.out
objdump -drwC -Mintel --no-show-raw-insn example.out > example.s
