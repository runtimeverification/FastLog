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
        // TODO: return my event buffer to buffer manager
    }

    int i;
};

thread_local Context context;

#endif //FASTLOG_CONTEXT_H
