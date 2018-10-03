#ifndef FASTLOG_EVENTBUFFER_H
#define FASTLOG_EVENTBUFFER_H

#include <cstdint>

struct EventBuffer {

    // Note: this Ref object is designed to make manual instrumentation easier.
    // When we need to implement it via instrumentation, we probably need to
    // stick to plain struct and global functions.

    /// A reference to the current `__log_buffer` that can get out-dated shortly.
    struct Ref {
        EventBuffer* logBuf;
        uint64_t* buf;
        int events;
        int checkAliveTime;

        ~Ref()
        {
            logBuf->events = events;
            logBuf->checkAliveTime = checkAliveTime;
        }

        void
        checkUpdate(EventBuffer* curBuf)
        {
            if (logBuf == curBuf) {
                return;
            }

            // DEBUG ONLY
            EventBuffer::totalEvents += events;
            // Write-back #events (and #events only) to the old event buffer.
            logBuf->events = events;

            // Create a "reference" to the new event buffer.
            logBuf = curBuf;
            buf = curBuf->buf;
            events = 0;
            checkAliveTime = EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
        }

    };

    explicit EventBuffer()
    {
        reset();
    }

    Ref
    getRef() {
       return {this, buf, events, 0};
    }

    void
    reset()
    {
        tid = -1;
        events = 0;
//        nextBufPtrReloadTime = BUFFER_PTR_RELOAD_PERIOD;
//        eventsToRdtsc = RDTSC_SAMPLING_RATE;
    }

    // FIXME: looks like the size of the log can affect quite a lot on the performance.
    // However, since the slow path at the end of each epoch, we can't use buffer
    // too small. Need to understand why. I don't think the size of buffer should
    // affect that much...

    // Each buffer can hold up to 10M events. Each event is 8-byte, so each buffer
    // is 80 MB. Suppose we can log events at the rate of ~1ns/event, we need 10ms
    // to fill up the buffer.
//  static const int MAX_EVENTS = 10000000;
    static const int MAX_EVENTS = 10000;

    /// Generate a timestamp event every X events.
    static const int RDTSC_SAMPLING_RATE = 128;
//  static const int RDTSC_SAMPLING_RATE = 32;

    /// TODO: choose this number empirically; we want the smallest number that
    /// doesn't affect performance.
    static const int BUFFER_PTR_RELOAD_PERIOD = 64;

    // # events stored in the buffer.
    int events;

    /// Time to check if this event buffer has been reclaimed
    /// (Re-)fetch the event buffer pointer using atomic operation when #events
    /// exceeds this number.
    int checkAliveTime;
//    // # events till we generate the next timestamp.
//    int eventsToRdtsc;

    // Note: not a ring buffer.
    uint64_t buf[MAX_EVENTS + BUFFER_PTR_RELOAD_PERIOD];

    /// Thread ID.
    int tid;

    static __thread int64_t totalEvents;
};

__thread int64_t EventBuffer::totalEvents = 0;

#endif //FASTLOG_EVENTBUFFER_H
