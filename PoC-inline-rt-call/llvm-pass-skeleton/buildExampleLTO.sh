#!/bin/bash
# Failed attempt to inline runtime functions using -flto, although I feel like
# it can be done in principle. The following commands follow the instructions
# at https://llvm.org/docs/LinkTimeOptimization.html.
OPT=-O3
# Compile the runtime library into LLVM bitcode.
clang $OPT -flto -c rtlib.c
# Instrument the application and generate the object file.
clang $OPT -Xclang -load -Xclang build/skeleton/libSkeletonPass.so -c example.c
# Use LTO to merge rtlib in bitcode and the application in native code.
# TODO: I expect `logop` to be inlined at this step; however, this doesn't seem
# to work.
clang $OPT -flto example.o rtlib.o -o exampleLTO.out
objdump -drwC -Mintel --no-show-raw-insn exampleLTO.out > exampleLTO.s
