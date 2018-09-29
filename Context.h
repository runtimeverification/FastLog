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
//    return __atomic_load_n(&__log_buffer, __ATOMIC_RELAXED);
}

#endif //FASTLOG_CONTEXT_H
