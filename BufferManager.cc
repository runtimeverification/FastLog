#include "BufferManager.h"
#include "Context.h"

/**
 * Called by application threads, as soon as they detected an epoch change,
 * to get a new event buffer for the latest epoch.
 */
void
BufferManager::allocBuffer()
{
    std::lock_guard<std::mutex> _(epochMutex);
    tlsBufAddrs.insert(&__log_buffer);
    __atomic_store_n(&__log_buffer, getFreshBuf(), __ATOMIC_RELEASE);
}

// TODO: voluntarily release is only useful upon thread_exit? And by worker
// threads to return event buffers?
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
    std::lock_guard<std::mutex> _(epochMutex);
    for (auto buf : *bufsToRelease) {
        freeBufs.push_back(buf);
    }
}

bool
BufferManager::tryIncEpoch(EventBuffer::Ref* ref)
{
    int oldEpoch = ref->logBuf->epoch;
    if (!epoch.compare_exchange_strong(oldEpoch, oldEpoch + 1)) {
        return false;
    }

    // We are the coordinator thread of `oldEpoch`. Start the final
    // synchronization stage of `oldEpoch`.
    std::lock_guard<std::mutex> _(epochMutex);

    // Reclaim all event buffers allocated in `oldEpoch`.
    for (auto tlsAddr : tlsBufAddrs) {
        reclaimedBufs.push_back(__atomic_load_n(tlsAddr, __ATOMIC_RELAXED));
        if (tlsAddr == &__log_buffer) {
            ref->checkUpdate(getFreshBuf());
        } else {
            __atomic_store_n(tlsAddr, NULL, __ATOMIC_RELAXED);
        }
    }
    tlsBufAddrs.clear();

    // FIXME: the following needs to happen in the worker thread; don't delay
    // this application thread.
    for (auto buf : reclaimedBufs) {
        while (!buf->closed) {}
    }
//    release(reclaimedBufs);
    for (auto buf : reclaimedBufs) {
        freeBufs.push_back(buf);
    }
    reclaimedBufs.clear();
    return true;
}

/**
 * Invoked by application threads to return their event buffer when they
 * are about to exit.
 */
void
BufferManager::threadExit()
{
    std::lock_guard<std::mutex> _(epochMutex);
    EventBuffer* buf = __atomic_load_n(&__log_buffer, __ATOMIC_RELAXED);
    if (buf != NULL) {
        if (buf->epoch == epoch) {
            reclaimedBufs.push_back(buf);
        } else {
            freeBufs.push_back(buf);
        }
        tlsBufAddrs.erase(&__log_buffer);
    }
    printf("thread exit\n");
}

/**
 * Helper method to retrieve a fresh event buffer. Attempt to reuse old This method is thread-unsafe.
 * Synchronization must be done by the caller.
 *
 * \return
 *      A fresh event buffer ready to use.
 */
EventBuffer*
BufferManager::getFreshBuf()
{
    EventBuffer* buf;
    if (freeBufs.empty()) {
        buf = new EventBuffer();
    } else {
        buf = freeBufs.back();
        freeBufs.pop_back();
        buf->reset();
    }
    buf->tid = __thr_context.tid;
    buf->epoch = epoch.load(std::memory_order_relaxed);
    return buf;
}
