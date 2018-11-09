#include "rtlib.h"

#if DEF_IN_HEADER
extern void logop(int i);
#else
void logop(int i) {
    printf("lopop timestamp: %llu\n", __rdtsc());
}
#endif
