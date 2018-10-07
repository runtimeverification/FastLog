#ifndef FASTLOG_EVENTBUFFER_H
#define FASTLOG_EVENTBUFFER_H

#include <atomic>
#include <cassert>
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
        int nextRdtscTime;

        explicit Ref(EventBuffer* logBuf)
            : logBuf(logBuf)
            , buf(logBuf->buf)
            , events(logBuf->events)
            , nextRdtscTime(logBuf->nextRdtscTime)
        {}

        ~Ref()
        {
            logBuf->events = events;
            logBuf->nextRdtscTime = nextRdtscTime;
        }

        /**
         * Invoked by application threads to update this reference to point to
         * their newly assigned event buffers once they realize the old ones
         * have been reclaimed.
         *
         * \param curBuf
         *      Current event buffer. Must be non-null.
         */
        void
        updateLogBuffer(EventBuffer* curBuf)
        {
            assert(curBuf && (curBuf != logBuf));

            // Write-back #events (and #events only) to the old event buffer.
            logBuf->events = events;
            logBuf->closed = true;

            // Attach ourselves to the new event buffer.
            logBuf = curBuf;
            buf = curBuf->buf;
            events = 0;
            nextRdtscTime = EventBuffer::BATCH_SIZE;
        }

    };

    explicit EventBuffer()
    {
        reset();
    }

    Ref
    getRef() {
       return Ref(this);
    }

    void
    reset()
    {
        events = 0;
        nextRdtscTime = BATCH_SIZE;
        threadId = -1;
        epoch = -1;
        closed = false;
    }

    // FIXME: looks like the size of the log can affect quite a lot on the performance.
    // However, since the slow path at the end of each epoch, we can't use buffer
    // too small. Need to understand why. I don't think the size of buffer should
    // affect that much...

    // Each buffer can hold up to 10M events. Each event is 8-byte, so each buffer
    // is 80 MB. Suppose we can log events at the rate of ~1ns/event, we need 10ms
    // to fill up the buffer.
    static const int MAX_EVENTS = 10000000;

    /// # events a small buffer can hold. Chosen to fit the small buffer in the
    /// L1 data cache (assuming ~32KB) entirely. Intended for benchmark only.
    static const int MAX_EVENTS_SMALL = 1000;

    /// TODO: choose this number empirically; we want the smallest number that
    /// doesn't affect performance.
    /// Generate a timestamp after logging this many other events.
    static const int BATCH_SIZE = 64;

    /// # bytes used to record an event.
    static const int EVENT_SIZE = 8;

    // # events stored in the buffer.
    int events;

    /// Time to generate a timestamp for the current batch of events.
    int nextRdtscTime;

    /// Buffer storage used to hold events.
    uint64_t buf[MAX_EVENTS + BATCH_SIZE + 1];
    // TODO: any benefit aligned to 64-byte cache line boundary?
//    alignas(64) uint64_t buf[MAX_EVENTS + BATCH_SIZE + 1];

    /// Identifier for the application thread this buffer is assigned to.
    int threadId;

    /// Epoch number when BufferManager assigned this buffer to the application
    /// thread.
    int epoch;

    /// True if the application thread will not write to this buffer anymore.
    std::atomic<bool> closed;
};

#endif //FASTLOG_EVENTBUFFER_H
