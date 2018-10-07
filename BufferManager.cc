#include <cassert>
#include <thread>
#include "BufferManager.h"
#include "Context.h"
#include "Worker.h"

#define LOCK(x) std::lock_guard<std::mutex> _(x)
#define DEBUG printf

/**
 * Invoked by application threads, as soon as they detected an epoch change,
 * to get a new event buffer for the current epoch. Once this function returns,
 * subsequent calls to getLogBuffer() will return the new event buffer.
 *
 * By calling this function, an application thread notifies the logging runtime
 * that it will participate in the current epoch (so its event buffer must be
 * reclaimed at the end of the epoch).
 *
 * \pre
 *      The caller's thread-local event buffer pointer must be NULL before
 *      invoking this function (i.e., getLogBuffer() will return NULL).
 * \return
 *      An empty event buffer ready to use.
 */
EventBuffer*
BufferManager::allocBuffer()
{
    LOCK(monitor);
    assert(__atomic_load_n(&__log_buffer, __ATOMIC_RELAXED) == NULL);

    // Obtain an empty event buffer. Attempt to reuse old ones if possible.
    EventBuffer* buf;
    if (freeBufs.empty()) {
        buf = new EventBuffer();
    } else {
        buf = freeBufs.back();
        freeBufs.pop_back();
        buf->reset();
    }
    buf->threadId = __thr_context.threadId;
    buf->epoch = epoch;

    // TODO: I am not entirely satisfied with all these updates. Easy to forget.
    // TODO: improve doc; update our records.
    __atomic_store_n(&__log_buffer, buf, __ATOMIC_RELAXED);
    __thr_context.logBuffer = buf;
    allocatedBufs.push_back(buf);
    tlsBufAddrs.insert(&__log_buffer);
    return buf;
}

/**
 * Invoked by RV-Predict worker threads to return event buffers they have
 * finished processing.
 *
 * \param bufsToRelease
 *      Event buffers to return.
 */
void
BufferManager::release(std::vector<EventBuffer*>* bufsToRelease)
{
    LOCK(monitor);
    activeWorkers--;
    for (auto buf : *bufsToRelease) {
        freeBufs.push_back(buf);
    }
}

/**
 * Invoked by application threads, as soon as their event buffers become full,
 * to increment the epoch number. When there are multiple threads trying to
 * increment the epoch number, only one will succeed.
 *
 * The one thread that succeeds is named the coordinator thread, which is
 * responsible for collecting the buffers of the previous epoch and passing
 * them to a worker thread for analysis.
 *
 * \param ref
 *      Event buffer reference of the calling thread.
 * \return
 *      True if this thread successfully increments the epoch number.
 */
bool
BufferManager::tryIncEpoch(EventBuffer::Ref* ref)
{
    LOCK(monitor);
    if (epoch != ref->logBuf->epoch) {
        return false;
    }

    // We are the coordinator thread. Reclaim all event buffers allocated in
    // this epoch by setting the "thread-local" event buffer pointers of all
    // participating threads (including ourselves) to NULL.
    for (auto tlsAddr : tlsBufAddrs) {
        __atomic_store_n(tlsAddr, NULL, __ATOMIC_RELAXED);
    }
    tlsBufAddrs.clear();

    // Fire-and-forget a worker thread.
    if (activeWorkers < MAX_WORKERS) {
        std::thread worker(workerMain, this, allocatedBufs);
        worker.detach();
        activeWorkers++;
    } else {
        DEBUG("Too many active workers. Skip processing epoch %d\n", epoch);
        freeBufs.insert(freeBufs.end(), allocatedBufs.begin(),
                allocatedBufs.end());
    }
    allocatedBufs.clear();

    // Start of the new epoch.
    epoch++;
    return true;
}

/**
 * Invoked by application threads to return their event buffer when they
 * are about to exit.
 */
void
BufferManager::threadExit()
{
    LOCK(monitor);
    DEBUG("thread %d exits\n", __thr_context.threadId);
    if (__thr_context.logBuffer) {
        __thr_context.logBuffer->closed = true;
    }
    tlsBufAddrs.erase(&__log_buffer);
}
