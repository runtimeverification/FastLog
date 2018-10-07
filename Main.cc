#include <cassert>
#include <thread>

#include "BufferManager.h"
#include "Context.h"
#include "LoggerConsts.h"
#include "Utils.h"

/// # times to (over)write the array.
static int numIterations = 1000;

enum LogOp {
    /// Do nothing. This is the baseline.
    NO_OP               = 1,

    /// Do nothing but disable the use of SSE instructions.
    NO_SSE              = 2,

    /// Call an empty log function; do not inline the function.
    FUNC_CALL           = 3,

    /// Log the address involved in the memory load/store.
    LOG_ADDR            = 4,

    /// Based on LOG_ADDR, optimize to avoid indirect access to the internal
    /// fields of EventBuffer.
    LOG_DIRECT_LOAD,

    /// Based on LOG_ADDR, prefetch log entries periodically.
    PREFETCH_LOG_ENTRY,

    /// Based on LOG_ADDR, use atomic load read the event buffer pointer.
    VOLATILE_BUF_PTR,

    /// Based on LOG_ADDR, use atomic load read the event buffer pointer.
    SLOPPY_BUF_PTR,

    /// Log the event header.
    LOG_HEADER,

    /// Log the value involved in the memory load/store.
    LOG_VALUE,

    /// Log the source code location of the memory load/store.
    LOG_SRC_LOC,

    /// A relatively full implementation by combining LOG_SRC_LOC and
    /// SLOPPY_BUF_PTR.
    LOG_FULL,

    /// A straightforward implementation of LOG_FULL (i.e. no inlining, combine
    /// LOG_SRC_LOC and VOLATILE_BUF_PTR, no prefetch) for comparison.
    LOG_FULL_NAIVE,

    /// Based on LOG_FULL, each thread contacts the buffer manager to get a new
    /// buffer when its current buffer is full (i.e. advancing to the next
    /// epoch).
    BUFFER_MANAGER,

    /// Generate RDTSC events periodically.
    LOG_TIMESTAMP,
};

std::string
opcodeToString(LogOp op)
{
    switch (op) {
    case NO_OP:                 return "NO_OP";
    case NO_SSE:                return "NO_SSE";
    case FUNC_CALL:             return "FUNC_CALL";
    case LOG_ADDR:              return "LOG_ADDR";
    case LOG_DIRECT_LOAD:       return "LOG_DIRECT_LOAD";
    case PREFETCH_LOG_ENTRY:    return "PREFETCH_LOG_ENTRY";
    case VOLATILE_BUF_PTR:      return "VOLATILE_BUF_PTR";
    case SLOPPY_BUF_PTR:        return "SLOPPY_BUF_PTR";
    case LOG_HEADER:            return "LOG_HEADER";
    case LOG_VALUE:             return "LOG_VALUE";
    case LOG_SRC_LOC:           return "LOG_SRC_LOC";
    case LOG_FULL:              return "LOG_FULL";
    case LOG_FULL_NAIVE:        return "LOG_FULL_NAIVE";
    case BUFFER_MANAGER:        return "BUFFER_MANAGER";
    case LOG_TIMESTAMP:         return "LOG_TIMESTAMP";
    default:
        char s[50] = {};
        std::sprintf(s, "Unknown LogOp(%d)", op);
        return s;
    }
}

__attribute__((noinline))
void
run_noop(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        (*addr) = i;
    }
}

__attribute__((noinline, target("no-sse")))
void
run_no_sse(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        (*addr) = i;
    }
}

__attribute__((noinline))
void
__tsan_write8_func(uint64_t pc, void* addr, uint64_t val)
{
    // Somehow if we don't escape "addr" here, calls to this function will be
    // optimized away regardless of the "noinline" attribute.
    escape(addr);
}

__attribute__((noinline, target("no-sse")))
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
 * Log just the memory access address into a per-thread buffer. Wrap around on
 * overflow.
 *
 * Note: in this experiment, the event buffer pointer never changes so the
 * compiler will transform the code to  load it once into a temporary local
 * variable.
 */
__attribute__((always_inline))
void __tsan_write8_log_addr(uint64_t pc, void* addr, uint64_t val)
{
    EventBuffer* logBuf = getLogBufferUnsafe();
    logBuf->buf[logBuf->events] = (uint64_t) addr;
    if (UNLIKELY(logBuf->events++ == EventBuffer::MAX_EVENTS)) {
        logBuf->events = 0;
    }
}

#if 0
/// TODO: not sure why the following is actually slower than above; the assembly
/// has one less instruction (10 vs. 11) but lower IPC; it does simplify the
/// access, right?
__attribute__((always_inline))
void __tsan_write8_log_addr2(uint64_t pc, void* addr, uint64_t val)
{
    EventBuffer* logBuf = getLogBufferUnsafe();
    (*logBuf->next) = (uint64_t) addr;
    if (UNLIKELY(logBuf->next++ == logBuf->end)) {
        logBuf->next = logBuf->buf;
    }
}
#endif

__attribute__((noinline, target("no-sse")))
void
run_log_addr(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_addr(__LINE__, addr, i);
        (*addr) = i;
    }
}

/**
 * Based on LOG_ADDR, remove indirect access to EventBuffer::{events, buf} at
 * the source level.
 *
 * Note: this is just an experiment for the sake of curiosity; the generated
 * code should be the same as LOG_ADDR.
 */
__attribute__((always_inline))
void __tsan_write8_log_addr_direct(uint64_t* const buf, int& events,
        uint64_t pc, void* addr, uint64_t val) {
    buf[events] = (uint64_t) addr;
    if (UNLIKELY(events++ == EventBuffer::MAX_EVENTS)) {
        events = 0;
    }
}

__attribute__((noinline, target("no-sse")))
void
run_log_addr_direct(int64_t* array, int length)
{
    int events = getLogBufferUnsafe()->events;
    uint64_t* const buf = getLogBufferUnsafe()->buf;

    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_addr_direct(buf, events, __LINE__, addr, i);
        (*addr) = i;
    }

    getLogBufferUnsafe()->events = events;
}

/**
 * Based on LOG_ADDR, prefetch log entries into all levels of cache, preparing
 * for the writes.
 *
 * The prefetch is implemented using gcc builtins. Since write-prefetch is only
 * available as an ISA extension, in order to make sure the builtin is actually
 * compiled into the PREFETCHW instruction (as opposed to normal read-prefetch),
 * use compiler option "-march={broadwell, skylake, etc.}" or "-mprfchw".
 */
__attribute__((always_inline))
void __tsan_write8_prefetch_log_entries(uint64_t pc, void* addr, uint64_t val)
{
    EventBuffer* logBuf = getLogBufferUnsafe();
    logBuf->buf[logBuf->events] = (uint64_t) addr;
    if (UNLIKELY(logBuf->events++ >= logBuf->checkAliveTime)) {
        if (UNLIKELY(logBuf->events >= EventBuffer::MAX_EVENTS)) {
            logBuf->checkAliveTime = EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
            logBuf->events = 0;
        }

        // TODO: honestly, I have no clue if this is the best way to do prefetch.
        // I observed some speedup compared to LOG_ADDR when buffer size is large,
        // that's all. I don't even know what performance metrics I should be
        // looking at to see how these SW prefetch change/improve the behavior/performance.

        // Prefetch log entries that will be written in some future period.
        // A cache line is usually 64-byte, which can hold 8 events.
        int eventsPerLine = 64 / EventBuffer::EVENT_SIZE;
        int prefetchCacheLines =
                EventBuffer::BUFFER_PTR_RELOAD_PERIOD / eventsPerLine;
        // FIXME: the optimal prefetch distance should be determined by system
        // memory latency and cycles per loop iteration (i.e., it has nothing
        // to do with BATCH_SIZE)
        int prefetchDist = eventsPerLine * prefetchCacheLines * 2;
        for (int i = 0; i < prefetchCacheLines; i++) {
            int entry = logBuf->events + prefetchDist + i * eventsPerLine;
            __builtin_prefetch(&logBuf->buf[entry], 1, 3);
        }
        logBuf->checkAliveTime += EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
    }
}

__attribute__((noinline, target("no-sse")))
void
run_prefetch_log_entries(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_prefetch_log_entries(__LINE__, addr, i);
        (*addr) = i;
    }
}

/**
 * Similar to LOG_ADDR, except that we use atomic operation to load the event
 * buffer pointer so that the compiler cannot hoist that into a temporary local
 * variable.
 */
__attribute__((always_inline))
void __tsan_write8_volatile_bufptr(uint64_t pc, void* addr, uint64_t val)
{
    EventBuffer* logBuf = getLogBuffer();
    logBuf->buf[logBuf->events] = (uint64_t) addr;
    if (UNLIKELY(logBuf->events++ == EventBuffer::MAX_EVENTS)) {
        logBuf->events = 0;
    }
}

__attribute__((noinline, target("no-sse")))
void
run_volatile_buffer_ptr(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_volatile_bufptr(__LINE__, addr, i);
        (*addr) = i;
    }
}

// FIXME: FIGURE OUT WHY USING NOINLINE WILL SLOW DOWN THE PERFORMANCE SIGNIFICANTLY.
// THE RESULTING HOT LOOP IS 12 INSN (AS OPPOSED TO 11)...
// __attribute__((noinline))
void
__tsan_write8_volatile_bufptr_opt_slow(EventBuffer::Ref* ref)
{
    // Most likely, the event buffer pointer has not changed. Update alive time
    // and check if the buffer is full anyway.
    ref->checkAliveTime += EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
    if (UNLIKELY(ref->events >= EventBuffer::MAX_EVENTS)) {
        // Note: the real implementation will not wrap around; instead, it will
        // contact the buffer manager to advance the epoch.
        ref->events = 0;
        ref->checkAliveTime = EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
    }

    // Get a new event buffer if our current one has been reclaimed.
    EventBuffer* curBuf = getLogBuffer();
    if (curBuf == NULL) {
        ref->updateLogBuffer(__buf_manager.allocBuffer());
    }
}

__attribute__((always_inline))
void
__tsan_write8_volatile_bufptr_opt(EventBuffer::Ref* ref, uint64_t pc,
        void* addr, uint64_t val)
{
    ref->buf[ref->events] = (uint64_t) addr;
    if (UNLIKELY(ref->events++ >= ref->checkAliveTime)) {
        __tsan_write8_volatile_bufptr_opt_slow(ref);
    }
}

__attribute__((noinline, target("no-sse")))
void
run_volatile_buffer_ptr_opt(int64_t* array, int length)
{
    EventBuffer::Ref bufRef = getLogBufferRef();

    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_volatile_bufptr_opt(&bufRef, __LINE__, addr, i);
        (*addr) = i;
    }
}

/**
 * (Ab)use the highest 4 bits of the address as the event header.
 */
__attribute__((always_inline))
void __tsan_write8_log_header(uint64_t pc, void* addr, uint64_t val)
{
    // Note: assigning the header as a byte by overwriting the highest byte
    // seems to be much slower than bit manipulation.
    EventBuffer* logBuf = getLogBufferUnsafe();
    logBuf->buf[logBuf->events] =
            TSAN_WRITE8 | (TSAN_HDR_ZERO_MASK & (uint64_t) addr);
    if (UNLIKELY(logBuf->events++ == EventBuffer::MAX_EVENTS)) {
        logBuf->events = 0;
    }
}

__attribute__((noinline, target("no-sse")))
void
run_log_header(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_header(__LINE__, addr, i);
        (*addr) = i;
    }
}

/**
 * (Ab)use the next 8 bits of the address to store the last byte of the value.
 */
__attribute__((always_inline))
void __tsan_write8_log_value(uint64_t pc, void* addr, uint64_t val)
{
    val = uint64_t((char) val) << 52;
    EventBuffer* logBuf = getLogBufferUnsafe();
    logBuf->buf[logBuf->events] =
            TSAN_WRITE8 | val | (TSAN_VAL_ZERO_MASK & (uint64_t)addr);
    if (UNLIKELY(logBuf->events++ >= EventBuffer::MAX_EVENTS)) {
        logBuf->events = 0;
    }
}

__attribute__((noinline, target("no-sse")))
void
run_log_value(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_value(__LINE__, addr, i);
        (*addr) = i;
    }
}

/**
 * (Ab)use the next 20 bits of the address to store a unique location ID (it's
 * just the last 20 bits of CALLERPC for now).
 */
__attribute__((always_inline))
void __tsan_write8_log_src_loc(uint64_t pc, void* addr, uint64_t val)
{
    uint64_t loc = (pc << 44) >> 4;
    val = uint64_t((char) val) << 32;
    EventBuffer* logBuf = getLogBufferUnsafe();
    logBuf->buf[logBuf->events] =
            TSAN_WRITE8 | loc | val | (TSAN_LOC_ZERO_MASK & (uint64_t) addr);
    // FIXME: move "++" into the previous state would be faster for this
    // experiment, but not for SLOPPY_BUF_PTR; for now, I am using post-inc
    // for the sake of uniformity.
    if (UNLIKELY(logBuf->events++ >= EventBuffer::MAX_EVENTS)) {
        logBuf->events = 0;
    }
}

__attribute__((noinline, target("no-sse")))
void
run_log_src_loc(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_src_loc(__LINE__, addr, i);
        (*addr) = i;
    }
}

__attribute__((always_inline))
void __tsan_write8_log_full(EventBuffer::Ref* ref, uint64_t pc, void* addr,
        uint64_t val)
{
    uint64_t loc = (pc << 44) >> 4;
    val = uint64_t((char) val) << 32;
    ref->buf[ref->events] =
            TSAN_WRITE8 | loc | val | (TSAN_LOC_ZERO_MASK & (uint64_t) addr);
    if (UNLIKELY(ref->events++ >= ref->checkAliveTime)) {
        __tsan_write8_volatile_bufptr_opt_slow(ref);
    }
}

__attribute__((noinline, target("no-sse")))
void
run_log_full(int64_t* array, int length)
{
    EventBuffer::Ref bufRef = getLogBufferRef();

    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_full(&bufRef, __LINE__, addr, i);
        (*addr) = i;
    }
}

__attribute__((noinline))
void __tsan_write8_log_full_naive(uint64_t pc, void* addr, uint64_t val)
{
    uint64_t loc = (pc << 44) >> 4;
    val = uint64_t((char) val) << 32;
    EventBuffer* logBuf = getLogBuffer();
    logBuf->buf[logBuf->events] =
            TSAN_WRITE8 | loc | val | (TSAN_LOC_ZERO_MASK & (uint64_t) addr);
    if (UNLIKELY(logBuf->events++ == EventBuffer::MAX_EVENTS)) {
        logBuf->events = 0;
    }
}

__attribute__((noinline, target("no-sse")))
void
run_log_full_naive(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_full_naive(__LINE__, addr, i);
        (*addr) = i;
    }
}

// __attribute__((noinline))
void
__tsan_write8_buf_manager_slow(EventBuffer::Ref* ref)
{
    // Most likely, the event buffer pointer has not changed. Update alive time
    // and check if the buffer is full anyway.
    ref->checkAliveTime += EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
    if (UNLIKELY(ref->events >= EventBuffer::MAX_EVENTS)) {
        __buf_manager.tryIncEpoch(ref);
        ref->updateLogBuffer(__buf_manager.allocBuffer());
        return;
    }

    // Get a new event buffer if our current one has been reclaimed.
    EventBuffer* curBuf = getLogBuffer();
    if (curBuf == NULL) {
        ref->updateLogBuffer(__buf_manager.allocBuffer());
    }
}

__attribute__((always_inline))
void __tsan_write8_buf_manager(EventBuffer::Ref* ref, uint64_t pc, void* addr,
        uint64_t val)
{
    uint64_t loc = (pc << 44) >> 4;
    val = uint64_t((char) val) << 32;
    // FIXME: I think we need to do checkAliveTime first before logging to
    // ensure cut consistency (Update: doesn't matter; can't achieve cut
    // consistency anyway without waiting until all threads ack'ed the end of
    // current epoch)
    ref->buf[ref->events] =
            TSAN_WRITE8 | loc | val | (TSAN_LOC_ZERO_MASK & (uint64_t) addr);
    if (UNLIKELY(ref->events++ >= ref->checkAliveTime)) {
        __tsan_write8_buf_manager_slow(ref);
    }
}

__attribute__((noinline, target("no-sse")))
void
run_buf_manager(int64_t* array, int length)
{
    // FIXME: what if __log_buffer is currently NULL?
    EventBuffer::Ref bufRef = getLogBufferRef();

    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_buf_manager(&bufRef, __LINE__, addr, i);
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
        case NO_SSE:
            run_no_sse(array, length);
            break;
        case FUNC_CALL:
            run_func(array, length);
            break;
        case LOG_ADDR:
            run_log_addr(array, length);
            break;
        case PREFETCH_LOG_ENTRY:
            run_prefetch_log_entries(array, length);
            break;
        case LOG_DIRECT_LOAD:
            run_log_addr_direct(array, length);
            break;
        case VOLATILE_BUF_PTR:
            run_volatile_buffer_ptr(array, length);
            break;
        case SLOPPY_BUF_PTR:
            run_volatile_buffer_ptr_opt(array, length);
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
        case LOG_FULL:
            run_log_full(array, length);
            break;
        case LOG_FULL_NAIVE:
            run_log_full_naive(array, length);
            break;
//        case LOG_TIMESTAMP:
//            run_log_timestamp(array, length);
//            break;
        case BUFFER_MANAGER:
            run_buf_manager(array, length);
            break;
        default:
            std::printf("Unknown LogOp %d\n", logOp);
            break;
    }
}

void
workerMain(int tid, LogOp logOp, int64_t* array, int length)
{
    // Note: without the buffer manager, __log_buffer will always point to the
    // same EventBuffer allocated here.
    if (logOp < BUFFER_MANAGER) {
        __log_buffer = new EventBuffer();
    }

    uint64_t startTime = rdtsc();

    // Repeat the experiment many times.
    for (int i = 0; i < numIterations; i++) {
        run(logOp, array, length);
    }

    uint64_t totalTime = rdtsc() - startTime;
    double numWriteOps = static_cast<double>(length) * numIterations * 1e-6;
    printf("threadId %d, writeOps %.2fM, cyclesPerWrite %.2f\n", tid,
            numWriteOps, totalTime / numWriteOps * 1e-6);
}

int main(int argc, char **argv) {
    // # threads writing.
    int numThreads = 1;

    // Size of the array for each thread.
    int length = 1000000;

    // Operation to perform when logging.
    LogOp logOp = BUFFER_MANAGER;
//    LogOp logOp = NO_OP;

    if (argc == 4) {
        numThreads = atoi(argv[1]);
        length = atoi(argv[2]);
        logOp = static_cast<LogOp>(atoi(argv[3]));
    }
    printf("numThreads %d, arrayLength %d, %s, bufferSize %d, "
           "checkAlivePeriod %d\n", numThreads, length,
            opcodeToString(logOp).c_str(), EventBuffer::MAX_EVENTS,
            EventBuffer::BUFFER_PTR_RELOAD_PERIOD);

    int64_t* array = new int64_t[numThreads * length];
    std::thread* workers[numThreads];
    for (int i = 0; i < numThreads; i++) {
        workers[i] = new std::thread(workerMain, i, logOp, array + i * length,
                length);
    }
    for (std::thread* worker : workers) {
        worker->join();
    }

    return 0;
}