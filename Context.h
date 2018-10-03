#ifndef FASTLOG_CONTEXT_H
#define FASTLOG_CONTEXT_H

#include "BufferManager.h"

// TODO: introduce a namespace __rvp_context?

// TODO: this is really an atomic pointer; I didn't use the atomic annotation for
// two reasons: 1) I need a thread-unsafe version of read in getLogBufferUnsafe
// 2) "__thread" requires trivial ctor and "thread_local" seems to result in
// worse code?
extern __thread EventBuffer* __log_buffer;

extern thread_local BufferManager __buf_manager;

// FIXME: make methods in this header static? meaning?

/// WARNING: Not thread-safe. If you are using this method, you'd better know
/// what you are doing and what the compiler could be doing...
inline EventBuffer*
getLogBufferUnsafe()
{
    return __log_buffer;
}

inline EventBuffer*
getLogBuffer()
{
    return __atomic_load_n(&__log_buffer, __ATOMIC_RELAXED);
}

// TODO: is it OK to use getLogBufferUnsafe to read __log_buffer? My worry is that
// with the current impl. after inlining X small methods we will end up with X
// atomic loads in the parent method, which ideally could use just one atomic load.
// TODO: perhaps the cleanest solution is to always make sure our instrumentation
// pass runs late enough (after inlining, write-combining, etc.)
inline EventBuffer::Ref
getLogBufferRef()
{
    return getLogBuffer()->getRef();
}

// TODO:
struct Context {
    Context()
        : tid(threadCounter.fetch_add(1))
    {
        printf("context initialized\n");
    }

    ~Context()
    {
        __buf_manager.threadExit();
        printf("context destroyed\n");
        // TODO: return my event buffer to buffer manager
    }

    const int tid;

    static std::atomic<int> threadCounter;
};

extern thread_local Context __thr_context;

#endif //FASTLOG_CONTEXT_H
