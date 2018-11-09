#!/bin/bash
OPT=-O3
# Compile the runtime library into LLVM bitcode.
clang $OPT -c -emit-llvm rtlib.c

# Instrument the application with calls to the runtime function and emit LLVM bitcode.
clang $OPT -Xclang -load -Xclang build/skeleton/libSkeletonPass.so -c -emit-llvm example.c

# Merge the application and runtime library into one bc file.
llvm-link example.bc rtlib.bc > example-rt.bc

# Optimize the combined bc file. The output bitcode file can be inspected using command
# `opt -S example-opt.bc | less`, which should prove that `logop` is properly inlined.
opt $OPT example-rt.bc -o example-opt.bc

# Generate the executable and disassemble it.
clang $OPT example-opt.bc -o exampleBC.out
objdump -drwC -Mintel --no-show-raw-insn exampleBC.out > exampleBC.s
