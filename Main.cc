#include <cassert>
#include <thread>

#include "Context.h"
#include "LoggerConsts.h"
#include "Utils.h"

enum LogOp {
    /// Do nothing. This is the baseline.
    NO_OP           = 1,

    /// Call an empty log function; do not inline the function.
    DUMMY_CALL      = 2,

    /// Increment an event counter. Don't let the compiler to optimize it away.
    INC_COUNTER     = 3,

    /// Log the address involved in the memory load/store.
    LOG_ADDR        = 4,

    /// Log the event header.
    LOG_HEADER      = 5,

    /// Log the value involved in the memory load/store.
    LOG_VALUE       = 6,

    /// Log the source code location of the memory load/store.
    LOG_SRC_LOC     = 7,

    /// Generate RDTSC events periodically.
    LOG_TIMESTAMP   = 8,

    /// Switch the buffer to log when epoch changes.
    LOG_EPOCH       = 9
};

__attribute__((noinline))
void
run_noop(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        (*addr) = i;
    }
}

__attribute__((noinline))
void __tsan_write8_func(uint64_t pc, void* addr, uint64_t val)
{
    // Do nothing.
}

__attribute__((noinline))
void
run_func(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_func(__LINE__, addr, i);
        (*addr) = i;
    }
}

/**
 * Increment an event counter. Make sure it's not optimized away or hoisted
 * out of the loop by the compiler.
 */
__attribute__((always_inline))
void __tsan_write8_inc_counter_inl(uint64_t pc, void* addr, uint64_t val)
{
    int* events = &getLogBuffer()->events;
    escape(events);
    (*events)++;
    clobber();
}

__attribute__((noinline))
void __tsan_write8_inc_counter(uint64_t pc, void* addr, uint64_t val)
{
    __tsan_write8_inc_counter_inl(pc, addr, val);
}

__attribute__((noinline))
void
run_inc_counter(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_inc_counter_inl(__LINE__, addr, i);
        (*addr) = i;
    }
}

/**
 * Log just the memory access address into a per-thread buffer. Wrap around on
 * overflow.
 */
__attribute__((always_inline))
void __tsan_write8_log_addr_inl(uint64_t pc, void* addr, uint64_t val)
{
    EventBuffer* logBuf = getLogBuffer();
    logBuf->buf[logBuf->events++] = (uint64_t) addr;
    if (UNLIKELY(logBuf->events == EventBuffer::MAX_EVENTS)) {
        logBuf->events = 0;
    }
}

__attribute__((noinline))
void __tsan_write8_log_addr(uint64_t pc, void* addr, uint64_t val)
{
    __tsan_write8_log_addr_inl(pc, addr, val);
}

__attribute__((noinline))
void
run_log_addr(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_addr_inl(__LINE__, addr, i);
        (*addr) = i;
    }
}

/**
 * (Ab)use the highest 4 bits of the address as the event header.
 */
__attribute__((always_inline))
void __tsan_write8_log_header_inl(uint64_t pc, void* addr, uint64_t val)
{
    // Note: for some reason, assigning the header as a byte by overwriting
    // the highest byte seems to be much slower than bit manipulation.
    EventBuffer* logBuf = getLogBuffer();
    logBuf->buf[logBuf->events++] =
            TSAN_WRITE8 | (TSAN_HDR_ZERO_MASK & (uint64_t) addr);
    if (UNLIKELY(logBuf->events == EventBuffer::MAX_EVENTS)) {
        logBuf->events = 0;
    }
}

__attribute__((noinline))
void __tsan_write8_header_addr(uint64_t pc, void* addr, uint64_t val)
{
    __tsan_write8_log_header_inl(pc, addr, val);
}

__attribute__((noinline))
void
run_log_header(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_header_inl(__LINE__, addr, i);
        (*addr) = i;
    }
}

/**
 * (Ab)use the next 8 bits of the address to store the last byte of the value.
 */
__attribute__((always_inline))
void __tsan_write8_log_value_inl(uint64_t pc, void* addr, uint64_t val)
{
    val = uint64_t((char) val) << 52;
    EventBuffer* logBuf = getLogBuffer();
    logBuf->buf[logBuf->events++] =
            TSAN_WRITE8 | val | (TSAN_VAL_ZERO_MASK & (uint64_t)addr);
    if (UNLIKELY(logBuf->events >= EventBuffer::MAX_EVENTS)) {
        logBuf->events = 0;
    }
}

__attribute__((noinline))
void __tsan_write8_log_value(uint64_t pc, void* addr, uint64_t val)
{
    __tsan_write8_log_value_inl(pc, addr, val);
}

__attribute__((noinline))
void
run_log_value(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_value_inl(__LINE__, addr, i);
        (*addr) = i;
    }
}

/**
 * (Ab)use the next 20 bits of the address to store a unique location ID (it's
 * just the last 20 bits of CALLERPC for now).
 */
__attribute__((always_inline))
void __tsan_write8_log_src_loc_inl(uint64_t pc, void* addr, uint64_t val)
{
    val = uint64_t((char) val) << 52;
    uint64_t loc = (pc << 44) >> 12;
    EventBuffer* logBuf = getLogBuffer();
    logBuf->buf[logBuf->events++] =
            TSAN_WRITE8 | val | loc | (TSAN_LOC_ZERO_MASK & (uint64_t) addr);
    if (UNLIKELY(logBuf->events >= EventBuffer::MAX_EVENTS)) {
        logBuf->events = 0;
    }
}

__attribute__((noinline))
void __tsan_write8_log_src_loc(uint64_t pc, void* addr, uint64_t val)
{
    __tsan_write8_log_src_loc_inl(pc, addr, val);
}

__attribute__((noinline))
void
run_log_src_loc(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_src_loc_inl(__LINE__, addr, i);
        (*addr) = i;
    }
}

/**
 * Log a timestamp event. Extracted from __tsan_write8_log_timestamp_inl
 * so it doesn't have to be inlined.
 */
void
__tsan_rdtsc()
{
    EventBuffer* logBuf = getLogBuffer();
    logBuf->buf[logBuf->events++] =
            TSAN_RDTSC | (TSAN_HDR_ZERO_MASK & rdtsc());
    logBuf->eventsToRdtsc = EventBuffer::RDTSC_SAMPLING_RATE;
    if (UNLIKELY(logBuf->events >= EventBuffer::MAX_EVENTS)) {
        logBuf->events = 0;
    }
}

/**
 * Generate a timestamp every RDTSC_SAMPLING_RATE events.
 */
__attribute__((always_inline))
void __tsan_write8_log_timestamp_inl(uint64_t pc, void* addr, uint64_t val)
{
    val = uint64_t((char) val) << 52;
    uint64_t loc = (pc << 44) >> 12;
    // FIXME: why is using gcc intrinsic so much slower than a plain load?
//  EventBuffer* logBuf = __atomic_load_n(&logBuf, __ATOMIC_ACQUIRE);
    EventBuffer* logBuf = getLogBuffer();
    logBuf->buf[logBuf->events++] =
            TSAN_WRITE8 | val | loc | (TSAN_LOC_ZERO_MASK & (uint64_t)addr);
    logBuf->eventsToRdtsc--;
    if (UNLIKELY(logBuf->eventsToRdtsc == 0)) {
        // Rip out the cumbersome slow path into a separate function so that we
        // don't have to inline them everywhere.
        __tsan_rdtsc();
    }
}

__attribute__((noinline))
void __tsan_write8_log_timestamp(uint64_t pc, void* addr, uint64_t val)
{
    __tsan_write8_log_timestamp_inl(pc, addr, val);
}

__attribute__((noinline))
void
run_log_timestamp(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_timestamp_inl(__LINE__, addr, i);
        (*addr) = i;
    }
}

/**
 * Write to an array of 64-bit integers sequentially. Manually instrumented
 * with calls to log the memory store operations.
 *
 * \param logOp
 *      Selects the log operation to perform.
 * \param array
 *      64-bit integer array.
 * \param length
 *      Length of the array.
 */
__attribute__((noinline))
void
run(LogOp logOp, int64_t* array, int length)
{
    switch (logOp) {
        case NO_OP:
            run_noop(array, length);
            break;
        case DUMMY_CALL:
            run_func(array, length);
            break;
        case INC_COUNTER:
            run_inc_counter(array, length);
            break;
        case LOG_ADDR:
            run_log_addr(array, length);
            break;
        case LOG_HEADER:
            run_log_header(array, length);
            break;
        case LOG_VALUE:
            run_log_value(array, length);
            break;
        case LOG_SRC_LOC:
            run_log_src_loc(array, length);
            break;
        case LOG_TIMESTAMP:
            run_log_timestamp(array, length);
            break;
//        case LOG_CHANGE_EPOCH:
//            run_log_change_epoch(array, length);
//            break;
        default:
            std::printf("Unknown LogOp %d\n", logOp);
            break;
    }
}

void
workerMain(LogOp logOp, int64_t* array, int length)
{
    // Run the experiment 1000 times.
#define MAX_ITER 1000
    for (int i = 0; i < MAX_ITER; i++) {
        run(logOp, array, length);
    }
}

int main(int argc, char **argv) {
    // # threads writing.
    int numThreads = 1;

    // Size of the array for each thread.
    int length = 1000000;

    // Operation to perform when logging.
    LogOp logOp = NO_OP;

    if (argc == 4) {
        logOp = static_cast<LogOp>(atoi(argv[1]));
        numThreads = atoi(argv[2]);
        length = atoi(argv[3]);
    }

    int64_t* array = new int64_t[numThreads * length];
    std::thread* workers[numThreads];
    for (int i = 0; i < numThreads; i++) {
        workers[i] = new std::thread(workerMain, logOp, array + i * length,
                length);
    }

    for (std::thread* worker : workers) {
        worker->join();
    }

    return 0;
}