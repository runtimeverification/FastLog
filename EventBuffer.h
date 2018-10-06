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
        int checkAliveTime;

        explicit Ref(EventBuffer* logBuf)
            : logBuf(logBuf)
            , buf(logBuf->buf)
            , events(logBuf->events)
            , checkAliveTime(logBuf->checkAliveTime)
        {}

        ~Ref()
        {
            logBuf->events = events;
            logBuf->checkAliveTime = checkAliveTime;
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
            checkAliveTime = EventBuffer::BUFFER_PTR_RELOAD_PERIOD;
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
        checkAliveTime = BUFFER_PTR_RELOAD_PERIOD;
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
//  static const int MAX_EVENTS = 10000000;
    static const int MAX_EVENTS = 10000;

    /// TODO: choose this number empirically; we want the smallest number that
    /// doesn't affect performance.
    static const int BUFFER_PTR_RELOAD_PERIOD = 64;

    // # events stored in the buffer.
    int events;

    /// Time to check if this event buffer has been reclaimed
    /// (Re-)fetch the event buffer pointer using atomic operation when #events
    /// exceeds this number.
    int checkAliveTime;

    // Note: not a ring buffer.
    uint64_t buf[MAX_EVENTS + BUFFER_PTR_RELOAD_PERIOD + 1];

    /// Identifier for the application thread this buffer is assigned to.
    int threadId;

    /// Epoch number when BufferManager assigned this buffer to the application
    /// thread.
    int epoch;

    /// True if the application thread will not write to this buffer anymore.
    std::atomic<bool> closed;
};

#endif //FASTLOG_EVENTBUFFER_H
