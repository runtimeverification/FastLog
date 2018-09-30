#ifndef FASTLOG_EVENTBUFFER_H
#define FASTLOG_EVENTBUFFER_H

#include <cstdint>

struct EventBuffer {
    explicit EventBuffer()
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

    // FIXME: looks like the size of the log can affect quite a lot on the performance.
    // However, since the slow path at the end of each epoch, we can't use buffer
    // too small. Need to understand why. I don't think the size of buffer should
    // affect that much...

    // Each buffer can hold up to 10M events. Each event is 8-byte, so each buffer
    // is 80 MB. Suppose we can log events at the rate of ~1ns/event, we need 10ms
    // to fill up the buffer.
//  static const int MAX_EVENTS = 10000000;
    static const int MAX_EVENTS = 1000;

    /// Generate a timestamp event every X events.
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
};

#endif //FASTLOG_EVENTBUFFER_H
