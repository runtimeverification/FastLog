// Mini-benchmark for tsan: non-shared memory writes.
// TODO: investigate why the executable compiled with g++ is so much faster than clang!
#include <cstdint>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_set>

int len;
int64_t *a;
const int kNumIter = 1000;

typedef unsigned char u8;
typedef unsigned long uptr;  // NOLINT
static_assert(sizeof(uptr) == 8);
#define CALLERPC ((uptr)__builtin_return_address(0))
#define LIKELY(x)     __builtin_expect(!!(x), 1)
#define UNLIKELY(x)   __builtin_expect(!!(x), 0)

#define INLINE_INSTRUMENTATION 1

#define INSTRUMENT_BODY_EMPTY   1
#define INSTRUMENT_BODY_COUNT   2
#define INSTRUMENT_BODY_ADDR    3
#define INSTRUMENT_BODY_DYNBUF  4
#define INSTRUMENT_BODY_HEADER  5
#define INSTRUMENT_BODY_VALUE   6
#define INSTRUMENT_BODY_LOCID   7
#define INSTRUMENT_BODY_RDTSC   8
#define INSTRUMENT_BODY_EPOCH   9

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

// TODO: Another problem is how to synchronize access to these buffers. Or,
// how should we pass buffers from app threads to worker threads. How does the
// worker thread knows how many events there are in a buffer? In a nutshell,
// we (i.e. EventBufferManager) cannot rely on the app threads to return the
// EventBuffer quickly (e.g. they can exit, be descheduled, don't call in to
// the runtime library without letting the EventBufferManager know); we must
// always be able to revoke the ownership of a buffer. So the app thread should
// check whether its access has been revoked every time it tries to log?

std::atomic<int> threadIdCounter(0);

struct ThreadState {
    const int tid;

    ThreadState()
        : tid(threadIdCounter.fetch_add(1))
    {}

    ~ThreadState()
    {
        // FIXME: ThreadState & EventBufferManager depend on each other; how to
        // allow/fix this?
//        bufferManager.release();
    }
};

// FIXME: can I use ctor/dtor of this variable to act as hook into thread_init & thread_exit?
thread_local ThreadState __thread_state;

/// Global epoch number. Used to select one coordinator thread at the end of
/// each epoch. The coordinator thread is then responsible to synchronize with
/// other app threads and, once it's done, to pass event buffers from this epoch
/// to a worker thread.
std::atomic<int> __global_epoch(0);


__attribute__((always_inline))
uint64_t
rdtsc()
{
  uint32_t lo, hi;
  __asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi));
  return (((uint64_t)hi << 32) | lo);
}

static void escape(void* p) {
    asm volatile("" : : "g"(p) : "memory");
}

static void clobber() {
    asm volatile("" : : : "memory");
}

struct EventBuffer {
  // FIXME: looks like the size of the log can affect quite a lot on the performance.
  // However, since the slow path at the end of each epoch, we can't use buffer
  // too small. Need to understand why. I don't think the size of buffer should
  // affect that much...

  // Each buffer can hold up to 10M events. Each event is 8-byte, so each buffer
  // is 80 MB. Suppose we can log events at the rate of ~1ns/event, we need 10ms
  // to fill up the buffer.
//  static const int MAX_EVENTS = 10000000;
  static const int MAX_EVENTS = 1000;

  static const int RDTSC_SAMPLING_RATE = 128;
//  static const int RDTSC_SAMPLING_RATE = 32;

  // Thread ID.
  int tid;

  // TODO: doc. set by EventBufferManager? also used to revoke ownership?
  int epoch;

  // # events stored in the buffer.
  int events;

  // # events till we generate the next timestamp.
  int eventsToRdtsc;

  // Note: not a ring buffer.
  uint64_t buf[MAX_EVENTS + RDTSC_SAMPLING_RATE];

  EventBuffer()
  {
      reset();
  }

  void
  reset()
  {
      tid = -1;
      epoch = -1;
      events = 0;
      eventsToRdtsc = RDTSC_SAMPLING_RATE;
  }
};

//thread_local std::atomic<EventBuffer*> __log_buffer;
__thread EventBuffer* __log_buffer;
//__thread std::atomic<EventBuffer*> __log_buffer;

// FIXME: I don't like the following static allocation. It's too rigid and
// inefficient. What if the app is a server that creates a new thread for each
// new request. There can be >> 256 threads in a long-running server. Besides,
// most of the threads are short-lived, so it doesn't make sense for them to
// always own max_epoch buffers. Why don't we keep a dynamic pool of 80MB
// buffers so that each app thread can acquire a new buffer when it detects a
// epoch change and each worker thread can release all buffers from the same
// epoch back to the pool once it finishes processing them.

// Statically allocated per-thread event buffers. Not practical; intended for
// benchmarking only.
std::array<EventBuffer, 8> __tsan_buffers = {};

class EventBufferManager {
  public:
    explicit EventBufferManager()
    {}

    /**
     * Invoked by the coordinator thread at the end of an epoch.
     */
    void
    endEpoch()
    {
        std::lock_guard<std::mutex> _(epochMutex);

        // Collect event buffers allocated in the previous epoch.
        for (auto addr : threadLocalBufs) {
            bufsInUse.push_back(__atomic_load_n(addr, __ATOMIC_RELAXED));
//            bufsInUse.push_back(addr->load(std::memory_order_relaxed));
        }

        // Reclaim event buffers allocated in the previous epoch. Spin long
        // enough to make sure cache copies are invalidated on other cores.
        // TODO: are 1000 cycles enough?
        uint64_t stopTime = rdtsc() + 1000;
        while (rdtsc() < stopTime) {
            for (auto addr : threadLocalBufs) {
                __atomic_store_n(addr, NULL, __ATOMIC_RELEASE);
//                addr->store(NULL);
            }
        }
        threadLocalBufs.clear();

        // TODO: actually pass them to a worker thread.
        for (EventBuffer* buf : bufsInUse) {
            buf->reset();
            freeBufs.push_back(buf);
        }
    }

    void threadExit()
    {
        std::lock_guard<std::mutex> _(epochMutex);
        EventBuffer* buf = __log_buffer;
//        EventBuffer* buf = __log_buffer.load(std::memory_order_relaxed);
        if (buf != NULL) {
            threadLocalBufs.erase(&__log_buffer);
            bufsInUse.push_back(buf);
        }
        printf("thread exit\n");
    }

    // TODO: voluntarily release is only useful upon thread_exit? And by worker
    // threads to return event buffers?
    void release(EventBuffer* buffer)
    {
        // TODO: sync. with endEpoch

    }

    void
    allocBuffer()
    {
        std::lock_guard<std::mutex> _(epochMutex);

        EventBuffer* buf;
        if (freeBufs.empty()) {
            buf = new EventBuffer();
        } else {
            buf = freeBufs.back();
            freeBufs.pop_back();
        }
        buf->tid = __thread_state.tid;
        buf->epoch = __global_epoch;

        __atomic_store_n(&__log_buffer, buf, __ATOMIC_RELEASE);
//        __log_buffer.store(buf);
        threadLocalBufs.insert(&__log_buffer);
    }

    // TODO: is std::mutex too slow? use SpinLock instead?
    /// Pro
    std::mutex epochMutex;

    /// Event buffers that are currently available.
    std::vector<EventBuffer*> freeBufs;

    /// Event buffers allocated to the current epoch.
    std::vector<EventBuffer*> bufsInUse;

    /// Pointers to the thread local `__log_buffer` of all threads that are
    /// participating in the current epoch.
    std::unordered_set<EventBuffer**> threadLocalBufs;
//    std::unordered_set<std::atomic<EventBuffer*>*> threadLocalBufs;

};

EventBufferManager bufferManager;

void
__tsan_rdtsc()
{
    EventBuffer* logBuf = __log_buffer;
//    EventBuffer* logBuf = __log_buffer.load(std::memory_order_relaxed);
    logBuf->buf[logBuf->events++] =
            TSAN_RDTSC | (TSAN_HDR_ZERO_MASK & rdtsc());
    logBuf->eventsToRdtsc = EventBuffer::RDTSC_SAMPLING_RATE;
    if (UNLIKELY(logBuf->events >= EventBuffer::MAX_EVENTS)) {
        logBuf->events = 0;
    }
}

// TODO: shall we pass epoch as arg; may increase the code size at call site by
// how many instructions?
void
__tsan_mem_access_slow()
{
    EventBuffer* logBuf = __atomic_load_n(&__log_buffer, __ATOMIC_RELAXED);
//    EventBuffer* logBuf = __log_buffer.load(std::memory_order_relaxed);

    // The buffer manager has reclaimed our event buffer. Ask for a new one.
    if (logBuf == NULL) {
        bufferManager.allocBuffer();
        return;
    }

    // Time to generate a new timestamp. Also check if the buffer has filled up.
    if (logBuf->eventsToRdtsc == 0) {
        logBuf->buf[logBuf->events++] =
                TSAN_RDTSC | (TSAN_HDR_ZERO_MASK & rdtsc());
        logBuf->eventsToRdtsc = EventBuffer::RDTSC_SAMPLING_RATE;

        // Our event buffer is full.
        if (UNLIKELY(logBuf->events >= EventBuffer::MAX_EVENTS)) {
            int epoch = logBuf->epoch;
            if (__global_epoch.compare_exchange_strong(epoch, epoch + 1)) {
                // TODO: debug print we are the coordinator.
//                printf("epoch = %d\n", epoch);
                // We are the coordinator thread for this finishing epoch.
                bufferManager.endEpoch();
            }
            bufferManager.allocBuffer();
        }
    }
}

#if INLINE_INSTRUMENTATION
__always_inline
#else
__attribute__((noinline))
#endif
void __tsan_write8(uptr pc, void* addr, uint64_t val)
{
#if INSTRUMENT_CHOICE == INSTRUMENT_BODY_EMPTY
  // Do nothing.
#elif INSTRUMENT_CHOICE == INSTRUMENT_BODY_COUNT
  // Count # events. Make sure the compiler doesn't optimize it away:
  // https://stackoverflow.com/questions/40122141/preventing-compiler-optimizations-while-benchmarking
  int* events = &__log_buffer->events;
  escape(events);
  (*events)++;
  clobber();
#elif INSTRUMENT_CHOICE == INSTRUMENT_BODY_ADDR
  // Log memory access address into a per-thread buffer. Wrap around on
  // overflow. Do not log values.
  __log_buffer->buf[__log_buffer->events++] = (uptr)addr;
  if (UNLIKELY(__log_buffer->events == EventBuffer::MAX_EVENTS)) {
    __log_buffer->events = 0;
  }
#elif INSTRUMENT_CHOICE == INSTRUMENT_BODY_DYNBUF
  // Use dynamically allocated buffer instead global static one.
  __log_buffer->buf[__log_buffer->events++] = (uptr)addr;
  if (UNLIKELY(__log_buffer->events == EventBuffer::MAX_EVENTS)) {
    __log_buffer->events = 0;
  }
#elif INSTRUMENT_CHOICE == INSTRUMENT_BODY_HEADER
  // (Ab)use the highest 4 bits of the address as the event header.
  // Note: for some reason, assigning the header as a byte by overwritting
  // the highest byte seems to be much slower than bit manipulation.
  __log_buffer->buf[__log_buffer->events++] =
          TSAN_WRITE8 | (TSAN_HDR_ZERO_MASK & (uptr)addr);
  if (UNLIKELY(__log_buffer->events == EventBuffer::MAX_EVENTS)) {
    __log_buffer->events = 0;
  }
#elif INSTRUMENT_CHOICE == INSTRUMENT_BODY_VALUE
  // (Ab)use the next 8 bits of the address to store the last byte of the
  // actual value.
  val = uint64_t((char) val) << 52;
  __log_buffer->buf[__log_buffer->events++] =
          TSAN_WRITE8 | val | (TSAN_VAL_ZERO_MASK & (uptr)addr);
  if (UNLIKELY(__log_buffer->events >= EventBuffer::MAX_EVENTS)) {
    __log_buffer->events = 0;
  }
#elif INSTRUMENT_CHOICE == INSTRUMENT_BODY_LOCID
  // (Ab)use the next 20 bits of the address to store a unique location ID
  // (it's just the last 20 bits of CALLERPC for now).
  val = uint64_t((char) val) << 52;
  uint64_t loc = (pc << 44) >> 12;
  __log_buffer->buf[__log_buffer->events++] =
          TSAN_WRITE8 | val | loc | (TSAN_LOC_ZERO_MASK & (uptr)addr);
  if (UNLIKELY(__log_buffer->events >= EventBuffer::MAX_EVENTS)) {
    __log_buffer->events = 0;
  }
#elif INSTRUMENT_CHOICE == INSTRUMENT_BODY_RDTSC
  // Generate a timestamp every RDTSC_SAMPLING_RATE events.
  val = uint64_t((char) val) << 52;
  uint64_t loc = (pc << 44) >> 12;
  // FIXME: why is using gcc intrinsic so much slower than a plain load?
//  EventBuffer* logBuf = __atomic_load_n(&__log_buffer, __ATOMIC_ACQUIRE);
  EventBuffer* logBuf = __log_buffer;
  logBuf->buf[logBuf->events++] =
          TSAN_WRITE8 | val | loc | (TSAN_LOC_ZERO_MASK & (uptr)addr);
  logBuf->eventsToRdtsc--;
  if (UNLIKELY(logBuf->eventsToRdtsc == 0)) {
    // Rip out the cumbersome slow path into a separate function so that we
    // don't have to inline them everywhere.
    __tsan_rdtsc();
  }
#elif INSTRUMENT_CHOICE == INSTRUMENT_BODY_EPOCH
  // The first thread whose event buffer becomes full is responsible for
  // advancing the epoch (note: make sure only one thread succeeds when there
  // are several concurrent attempts) and collecting event buffers from the
  // previous epoch for processing.
  val = uint64_t((char) val) << 52;
  uint64_t loc = (pc << 44) >> 12;

  // TODO: can we use relaxed order here if we are using busy-wait/write in the
  // master thread for sync. purpose anyway. Test if acquire actually slows down
  // the execution.
//  EventBuffer* logBuf = __log_buffer.load(std::memory_order_acquire);
//  EventBuffer* logBuf = __atomic_load_n(&__log_buffer, __ATOMIC_ACQUIRE);
  EventBuffer* logBuf = __log_buffer;
  if (LIKELY(logBuf)) {
      logBuf->buf[logBuf->events++] =
              TSAN_WRITE8 | val | loc | (TSAN_LOC_ZERO_MASK & (uptr)addr);
      logBuf->eventsToRdtsc--;
  }

  if (UNLIKELY(!logBuf || (logBuf->eventsToRdtsc == 0))) {
    // Rip out the cumbersome slow path into a separate function so that we
    // don't have to inline them everywhere.
    __tsan_mem_access_slow();
  }
#else
  #error Unsupported choice setting
#endif
}

__attribute__((noinline))
void Run(int idx) {
  for (int64_t i = 0, n = len; i < n; i++) {
    int64_t* addr = &a[i + idx * n];
    __tsan_write8(__LINE__, addr, i);
    (*addr) = i;
  }
}

void *Thread(void *arg) {
  long idx = (long)arg;
  printf("Thread %ld started\n", idx);
#if INSTRUMENT_CHOICE >= INSTRUMENT_BODY_DYNBUF
  bufferManager.allocBuffer();
#else
  __log_buffer = &__tsan_buffers.at(idx);
#endif
  for (int i = 0; i < kNumIter; i++)
    Run(idx);
  printf("Thread %ld done\n", idx);
  return 0;
}

int main(int argc, char **argv) {
  int n_threads = 0;
  if (argc != 3) {
    n_threads = 4;
    len = 1000000;
  } else {
    n_threads = atoi(argv[1]);
    assert(n_threads > 0 && n_threads <= 32);
    len = atoi(argv[2]);
  }
  printf("%s: n_threads=%d len=%d iter=%d\n",
         __FILE__, n_threads, len, kNumIter);
  a = new int64_t[n_threads * len];
  pthread_t *t = new pthread_t[n_threads];
  for (int i = 0; i < n_threads; i++) {
    pthread_create(&t[i], 0, Thread, (void*)i);
  }
  for (int i = 0; i < n_threads; i++) {
    pthread_join(t[i], 0);
  }
  delete [] t;
  delete [] a;
  return 0;
}
