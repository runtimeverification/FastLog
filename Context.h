#ifndef FASTLOG_CONTEXT_H
#define FASTLOG_CONTEXT_H

#include "EventBuffer.h"

__thread EventBuffer* __log_buffer;

// FIXME: make it static? meaning?
__attribute__((always_inline))
EventBuffer*
getLogBuffer()
{
    return __log_buffer;
}

__attribute__((always_inline))
EventBuffer*
getLogBufferAtomic()
{
    return __atomic_load_n(&__log_buffer, __ATOMIC_RELAXED);
}

// TODO:
struct Context {
    Context()
    {
        printf("context initialized\n");
    }

    ~Context()
    {
        printf("context destroyed\n");
    }

    int i;
};

thread_local Context context;

struct LocalContext {
    EventBuffer* logBuf;
    int events;
    int refetchCounter;
    uint64_t* buf;

    LocalContext()
        : logBuf(getLogBuffer())
        , events(logBuf->events)
        , refetchCounter(logBuf->refetchCounter)
        , buf(logBuf->buf)
    {}

    ~LocalContext()
    {
        flush();
    }

    void flush()
    {
        if (logBuf) {
            logBuf->events = events;
            logBuf->refetchCounter = refetchCounter;
        }
    }

    /// Flush and reload.
    void update()
    {
        refetchCounter = 0;
        flush();
        EventBuffer* oldLogBuf = logBuf;
        logBuf = getLogBufferAtomic();
        if (logBuf && (logBuf != oldLogBuf)) {
            events = logBuf->events;
            refetchCounter = logBuf->refetchCounter;
            buf = logBuf->buf;
        }
    }
};

#endif //FASTLOG_CONTEXT_H
