#include <cassert>
#include <thread>

#include "Context.h"
#include "LoggerConsts.h"
#include "Utils.h"

/// # times to (over)write the array.
static int numIterations = 1000;

enum LogOp {
    /// Do nothing. This is the baseline.
    NO_OP               = 1,

    /// Call an empty log function; do not inline the function.
    FUNC_CALL           = 2,

    /// Log the address involved in the memory load/store.
    LOG_ADDR            = 3,

    /// Based on LOG_ADDR, optimize to avoid indirect access to the internal
    /// fields of EventBuffer.
    LOG_DIRECT_LOAD,

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

    /// Based on LOG_ADDR, use atomic load to prevent compiler from hoisting
    /// the event buffer pointer to a constant local variable.
    VOLATILE_BUF,

    /// Optimize VOLATILE_BUF by only doing the atomic load periodically.
    VOLATILE_BUF_OPT,

    /// Generate RDTSC events periodically.
    LOG_TIMESTAMP,

    /// Switch the buffer to log when epoch changes.
    LOG_EPOCH,
};

std::string
opcodeToString(LogOp op)
{
    switch (op) {
    case NO_OP:                 return "NO_OP";
    case FUNC_CALL:             return "FUNC_CALL";
    case LOG_ADDR:              return "LOG_ADDR";
    case LOG_DIRECT_LOAD:       return "LOG_DIRECT_LOAD";
    case VOLATILE_BUF_PTR:      return "VOLATILE_BUF_PTR";
    case SLOPPY_BUF_PTR:        return "SLOPPY_BUF_PTR";
    case LOG_HEADER:            return "LOG_HEADER";
    case LOG_VALUE:             return "LOG_VALUE";
    case LOG_SRC_LOC:           return "LOG_SRC_LOC";
    case LOG_TIMESTAMP:         return "LOG_TIMESTAMP";
    default:
        char s[50] = {};
        std::sprintf(s, "Unknown LogOp(%d)", op);
        return s;
    }
}

__attribute__((noinline, target("no-sse")))
void
run_noop(int64_t* array, int length)
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
    EventBuffer* logBuf = getLogBuffer();
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
    EventBuffer* logBuf = getLogBuffer();
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
    int events = getLogBuffer()->events;
    uint64_t* const buf = getLogBuffer()->buf;

    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_addr_direct(buf, events, __LINE__, addr, i);
        (*addr) = i;
    }

    getLogBuffer()->events = events;
}

/**
 * Similar to LOG_ADDR, except that we use atomic operation to load the event
 * buffer pointer so that the compiler cannot hoist that into a temporary local
 * variable.
 */
__attribute__((always_inline))
void __tsan_write8_volatile_bufptr(uint64_t pc, void* addr, uint64_t val)
{
    EventBuffer* logBuf = getLogBufferAtomic();
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

/**
 * TODO: based on above; try to optimize it to an extreme!!!
 */
 /*
__attribute__((always_inline))
void
__tsan_write8_volatile_bufptr_opt(EventBuffer*& logBuf, uint64_t pc, void* addr,
        uint64_t val)
{
    logBuf->buf[logBuf->events] = (uint64_t) addr;
    if (UNLIKELY(logBuf->events++ >= logBuf->nextBufPtrReloadTime)) {
        logBuf->nextBufPtrReloadTime += EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
        if (UNLIKELY(logBuf->events >= EventBuffer::MAX_EVENTS)) {
            logBuf->events = 0;
            logBuf->nextBufPtrReloadTime = EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
        }
        logBuf = getLogBufferAtomic();
    }
//    if (UNLIKELY(logBuf->eventsToRdtsc-- == 0)) {
//        logBuf = getLogBufferAtomic();
//        logBuf->eventsToRdtsc = EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
//        if (UNLIKELY(logBuf->events >= EventBuffer::MAX_EVENTS)) {
//            logBuf->events = 0;
//        }
////        EventBuffer* old = logBuf;
////        logBuf = getLogBufferAtomic();
////        if (old == logBuf) {
////            logBuf->eventsToRdtsc = EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
////            if (UNLIKELY(logBuf->events >= EventBuffer::MAX_EVENTS)) {
////                logBuf->events = 0;
////            }
////        }
//    }
}
  */


// FIXME: FIGURE OUT WHY USING NOINLINE WILL SLOW DOWN THE PERFORMANCE SIGNIFICANTLY.
// THE RESULTING HOT LOOP IS 12 INSN (AS OPPOSED TO 11)...
// __attribute__((noinline))
void
__tsan_write8_volatile_bufptr_opt_slow(EventBuffer*& logBuf, uint64_t*& buf,
        int &events, int &reloadTime)
{
    reloadTime += EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
    if (UNLIKELY(events >= EventBuffer::MAX_EVENTS)) {
        EventBuffer::totalEvents += events; // DEBUG ONLY
        events = 0;
        reloadTime = EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
    }
    EventBuffer* old = logBuf;
    logBuf = getLogBufferAtomic();
    if (UNLIKELY(old != logBuf)) {
        // Write-back.
        old->events = events;
        // Create a "reference" to the new event buffer.
        buf = logBuf->buf;
        events = 0;
        reloadTime = EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
        // DEBUG ONLY
        EventBuffer::totalEvents += events;
    }
}

__attribute__((always_inline))
void
__tsan_write8_volatile_bufptr_opt(EventBuffer*& logBuf, uint64_t*& buf,
        int& events, int& reloadTime, uint64_t pc, void* addr, uint64_t val)
{
    buf[events] = (uint64_t) addr;
    if (UNLIKELY(events++ >= reloadTime)) {
        __tsan_write8_volatile_bufptr_opt_slow(logBuf, buf, events, reloadTime);
    }
}

__attribute__((noinline, target("no-sse")))
void
run_volatile_buffer_ptr_opt(int64_t* array, int length)
{
    // TODO: can we get rid of this?
    EventBuffer* logBuf = getLogBuffer();
    int events = logBuf->events;
    int reloadTime = logBuf->nextBufPtrReloadTime;
    uint64_t* buf = logBuf->buf;

    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_volatile_bufptr_opt(logBuf, buf, events, reloadTime,
                __LINE__, addr, i);
//        __tsan_write8_volatile_bufptr_opt(__LINE__, addr, i);
        (*addr) = i;
    }

    // write-back
    getLogBuffer()->events = events;
    getLogBuffer()->nextBufPtrReloadTime = reloadTime;
}


/**
 * (Ab)use the highest 4 bits of the address as the event header.
 */
__attribute__((always_inline))
void __tsan_write8_log_header(uint64_t pc, void* addr, uint64_t val)
{
    // Note: assigning the header as a byte by overwriting the highest byte
    // seems to be much slower than bit manipulation.
    EventBuffer* logBuf = getLogBuffer();
    logBuf->buf[logBuf->events++] =
            TSAN_WRITE8 | (TSAN_HDR_ZERO_MASK & (uint64_t) addr);
    if (UNLIKELY(logBuf->events == EventBuffer::MAX_EVENTS)) {
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
    EventBuffer* logBuf = getLogBuffer();
    logBuf->buf[logBuf->events++] =
            TSAN_WRITE8 | val | (TSAN_VAL_ZERO_MASK & (uint64_t)addr);
    if (UNLIKELY(logBuf->events >= EventBuffer::MAX_EVENTS)) {
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
    val = uint64_t((char) val) << 52;
    uint64_t loc = (pc << 44) >> 12;
    EventBuffer* logBuf = getLogBuffer();
    logBuf->buf[logBuf->events++] =
            TSAN_WRITE8 | val | loc | (TSAN_LOC_ZERO_MASK & (uint64_t) addr);
    if (UNLIKELY(logBuf->events >= EventBuffer::MAX_EVENTS)) {
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

/**
 * FIXME: the doc. is out-dated.
 *
 * Similar to __tsan_write8_log_addr, except that we use atomic operation to
 * load the thread local event buffer pointer so that the compiler cannot hoist
 * it into a constant local variable.
 */
__attribute__((always_inline))
void __tsan_write8_volatile_buf(uint64_t pc, void* addr, uint64_t val)
{
    // FIXME: this is not very interesting; the generated asm is just one insn
    // loading the thread_local variable into a unused register; since there is
    // no data dependency (I guess) the resulting asm is super-fast...
    getLogBufferAtomic();
}

__attribute__((noinline, target("no-sse")))
void
run_volatile_buffer(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_volatile_buf(__LINE__, addr, i);
        (*addr) = i;
    }
}

// FIXME: the following implementation is WRONG! To the compiler, the updated
// logBuf is an entirely different local var. (or, register)!
/**
 * Attempts to optimize __tsan_write8_volatile_buf by only doing the atomic load
 * periodically. As a result, after inlining, the compiler will still hoist
 * `getLogBuffer` to a local variable; however, now the variable can be updated
 * by `getLogBufferAtomic` periodically.
 */
__attribute__((always_inline))
void __tsan_write8_volatile_buf_sloppy(LocalContext* ctx, uint64_t pc, void* addr, uint64_t val)
//void __tsan_write8_volatile_buf_sloppy(EventBuffer*& logBuf, int& events,
//        uint64_t*& buf, uint64_t pc, void* addr, uint64_t val)
{
    ctx->buf[ctx->events++] = (uint64_t)addr;
    ctx->refetchCounter++;
    if (UNLIKELY(ctx->refetchCounter == EventBuffer::BUFFER_PTR_RELOAD_PERIOD)) {
        ctx->update();
        if (ctx->events >= EventBuffer::MAX_EVENTS) {
            ctx->events = 0;
        }
    }
//    buf[events++] = (uint64_t)addr;
//    if (UNLIKELY(events == EventBuffer::BUFFER_PTR_RELOAD_PERIOD)) {
//        // Flush cache copies.
//        logBuf->events = events;
//
//        // Re-fetch our buffer pointer.
//        logBuf = getLogBufferAtomic();
//
//        // Populate cache again.
//        events = 0;
////        events = logBuf->events;
//        buf = logBuf->buf;
//    }
}
//__attribute__((always_inline))
//void __tsan_write8_volatile_buf_sloppy(EventBuffer*& logBuf, uint64_t pc,
//        void* addr, uint64_t val)
//{
//    logBuf->buf[logBuf->events++] = (uint64_t)addr;
//    if (UNLIKELY(logBuf->events == EventBuffer::BUFFER_PTR_RELOAD_PERIOD)) {
//        logBuf = getLogBufferAtomic();
//        logBuf->events = 0;
//    }
//}

__attribute__((noinline, target("no-sse")))
void
run_volatile_buffer_sloppy(int64_t* array, int length)
{
//    // TODO: Note: this requires extra instrumentation support.
//    EventBuffer* logBuf = getLogBuffer();
//    // TODO: introduce local copy of logBuf->{events, buf}
//    int events = logBuf->events;
//    uint64_t* buf = logBuf->buf;
    LocalContext localContext;

    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_volatile_buf_sloppy(&localContext, __LINE__, addr, i);
//        __tsan_write8_volatile_buf_sloppy(localContext, __LINE__, addr, i);
//        __tsan_write8_volatile_buf_sloppy(logBuf, __LINE__, addr, i);
        (*addr) = i;
    }

//    // Flush local copies
//    logBuf->events = events;
}

__attribute__((always_inline))
void __tsan_write8_fast(uint64_t pc, void* addr, uint64_t val)
{
    EventBuffer* logBuf = getLogBuffer();
    static __thread int events;
    static __thread int eventsToAtomicLoad;
    static __thread uint64_t* buf;
    buf = logBuf->buf;

    buf[events++] = (uint64_t)addr;
    eventsToAtomicLoad++;
    if (UNLIKELY(eventsToAtomicLoad == EventBuffer::BUFFER_PTR_RELOAD_PERIOD)) {
        logBuf->events = events;
        logBuf = getLogBufferAtomic();
        events = logBuf->events;
        buf = logBuf->buf;
    }
}

__thread int __events;
__thread int __eventsToAtomicLoad;
__thread uint64_t* __buf;

__attribute__((noinline, target("no-sse")))
void
run_fast(int64_t* array, int length)
{
//    int events = __events;
//    int eventsToAtomicLoad = __eventsToAtomicLoad;
//    uint64_t* buf = __buf;

    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
#if 1
        EventBuffer* logBuf = getLogBuffer();
        logBuf->buf[__events++] = (uint64_t)addr;
        if (UNLIKELY(__events == EventBuffer::BUFFER_PTR_RELOAD_PERIOD)) {
            // Flush the cached copies.
            logBuf->events = __events;

            // Re-fetch the event buffer pointer. Might get a different one.
            logBuf = getLogBufferAtomic();

            // Re-populate the cache.
            __events = logBuf->events;

        }
#else
        __tsan_write8_fast(__LINE__, addr, i);
#endif
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
    // TODO: uncomment this
//    logBuf->eventsToRdtsc = EventBuffer::RDTSC_SAMPLING_RATE;
    if (UNLIKELY(logBuf->events >= EventBuffer::MAX_EVENTS)) {
        logBuf->events = 0;
    }
}

/**
 * Generate a timestamp every RDTSC_SAMPLING_RATE events.
 */
__attribute__((always_inline))
void __tsan_write8_log_timestamp(uint64_t pc, void* addr, uint64_t val)
{
    val = uint64_t((char) val) << 52;
    uint64_t loc = (pc << 44) >> 12;
    // FIXME: why is using gcc intrinsic so much slower than a plain load?
//  EventBuffer* logBuf = __atomic_load_n(&logBuf, __ATOMIC_ACQUIRE);
    EventBuffer* logBuf = getLogBuffer();
    logBuf->buf[logBuf->events++] =
            TSAN_WRITE8 | val | loc | (TSAN_LOC_ZERO_MASK & (uint64_t)addr);
    // TODO: uncomment this
//    logBuf->eventsToRdtsc--;
//    if (UNLIKELY(logBuf->eventsToRdtsc == 0)) {
//        // Rip out the cumbersome slow path into a separate function so that we
//        // don't have to inline them everywhere.
//        __tsan_rdtsc();
//    }
}

__attribute__((noinline, target("no-sse")))
void
run_log_timestamp(int64_t* array, int length)
{
    for (int i = 0; i < length; i++) {
        int64_t* addr = &array[i];
        __tsan_write8_log_timestamp(__LINE__, addr, i);
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
        case FUNC_CALL:
            run_func(array, length);
            break;
        case LOG_ADDR:
            run_log_addr(array, length);
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
//        case LOG_HEADER:
//            run_log_header(array, length);
//            break;
//        case LOG_VALUE:
//            run_log_value(array, length);
//            break;
//        case LOG_SRC_LOC:
//            run_log_src_loc(array, length);
//            break;
//        case VOLATILE_BUF:
//            run_volatile_buffer(array, length);
//            break;
//        case VOLATILE_BUF_OPT:
//            run_volatile_buffer_sloppy(array, length);
//            break;
//        case LOG_TIMESTAMP:
//            run_log_timestamp(array, length);
//            break;
//        case LOG_CHANGE_EPOCH:
//            run_log_change_epoch(array, length);
//            break;
        default:
            std::printf("Unknown LogOp %d\n", logOp);
            break;
    }
}

void
workerMain(int tid, LogOp logOp, int64_t* array, int length)
{
    // Hack: only log_change_epoch contains code to handle NULL __log_buffer.
    if (logOp <= LOG_TIMESTAMP) {
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
    EventBuffer::totalEvents += getLogBufferAtomic()->events;
    std::printf("Logged %ld events\n", EventBuffer::totalEvents);
}

int main(int argc, char **argv) {
    // # threads writing.
    int numThreads = 1;

    // Size of the array for each thread.
    int length = 1000000;

    // Operation to perform when logging.
    LogOp logOp = NO_OP;

    if (argc == 4) {
        numThreads = atoi(argv[1]);
        length = atoi(argv[2]);
        logOp = static_cast<LogOp>(atoi(argv[3]));
    }
    printf("numThreads %d, arrayLength %d, %s, bufferSize %d\n", numThreads,
            length, opcodeToString(logOp).c_str(), EventBuffer::MAX_EVENTS);

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