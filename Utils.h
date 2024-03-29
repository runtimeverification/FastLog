#ifndef FASTLOG_UTILS_H
#define FASTLOG_UTILS_H

#include <cstdint>

#define LIKELY(x)     __builtin_expect(!!(x), 1)
#define UNLIKELY(x)   __builtin_expect(!!(x), 0)

inline void
escape(void* p)
{
    asm volatile("" : : "g"(p) : "memory");
}

inline void
clobber()
{
    asm volatile("" : : : "memory");
}

inline uint64_t
rdtsc()
{
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi));
    return (((uint64_t)hi << 32) | lo);
}

#endif //FASTLOG_UTILS_H
