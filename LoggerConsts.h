#ifndef FASTLOG_LOGGER_H
#define FASTLOG_LOGGER_H

#include <cstdint>

// FIXME: what does static global mean in header again?

/// Layout of a 64-bit event
/// Header: 4 bit
/// SrcLoc: 20 bit (~1M)
/// Value: 8 bit (last byte of actual value)
/// Address: 32 bit (address space is 48-bit)

static const uint64_t TSAN_HDR_ZERO_MASK = ~(((uint64_t) 0b1111) << 60);
static const uint64_t TSAN_VAL_ZERO_MASK = ~(((uint64_t) 0b111111111111) << 52);
static const uint64_t TSAN_LOC_ZERO_MASK = ~(((uint64_t) 0xFFFFFFFF) << 32);

// isMemAcc = 0, eventType = 001
static const uint64_t TSAN_RDTSC = ((uint64_t) 0b0001) << 60;
// isMemAcc = 1, isWrite = 1, accessSizeLog = 0
static const uint64_t TSAN_WRITE1 = ((uint64_t) 0b1100) << 60;
// isMemAcc = 1, isWrite = 1, accessSizeLog = 1
static const uint64_t TSAN_WRITE2 = ((uint64_t) 0b1101) << 60;
// isMemAcc = 1, isWrite = 1, accessSizeLog = 2
static const uint64_t TSAN_WRITE4 = ((uint64_t) 0b1110) << 60;
// isMemAcc = 1, isWrite = 1, accessSizeLog = 3
static const uint64_t TSAN_WRITE8 = ((uint64_t) 0b1111) << 60;

#endif //FASTLOG_LOGGER_H
