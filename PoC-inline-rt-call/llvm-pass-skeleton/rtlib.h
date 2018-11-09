#include <stdio.h>
#include <x86intrin.h>

/// If DEF_IN_HEADER is 1, the content of rtlib.{h,c} is organized based on the
/// suggestion at:
/// https://stackoverflow.com/questions/10291581/how-to-properly-inline-for-static-libraries
/// However, I think it's irrelevant to us since we are compiling, merging, and
/// optimizing *.bc files by ourselves.
#define DEF_IN_HEADER 0

#if DEF_IN_HEADER
inline void logop(int i) {
    printf("lopop timestamp: %llu\n", __rdtsc());
}
#else
extern void logop(int i);
#endif
