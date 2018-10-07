#ifndef FASTLOG_BUFFERMANAGER_H
#define FASTLOG_BUFFERMANAGER_H

#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_set>

#include "EventBuffer.h"
#include "Utils.h"

/// TODO: doc
class BufferManager {
  public:
    explicit BufferManager()
        : monitor()
        , activeWorkers()
        , epoch(0)
        , allocatedBufs()
        , freeBufs()
        , tlsBufAddrs()
    {}

    EventBuffer* allocBuffer();
    void release(std::vector<EventBuffer*>* bufsToRelease);
    bool tryIncEpoch(EventBuffer::Ref* ref);
    void threadExit();

  private:
    /// Provides monitor-style synchronization for this class, effectively
    /// serializing all member function calls.
    std::mutex monitor;

    /// # worker threads currently active.
    int activeWorkers;

    /// Definitive truth of our current epoch.
    int epoch;

    /// Event buffers allocated in the current epoch. Must be passed to a worker
    /// thread for processing at the end of the epoch. Always empty at the
    /// beginning of a new epoch.
    std::vector<EventBuffer*> allocatedBufs;

    /// Pool of event buffers that are currently available.
    std::vector<EventBuffer*> freeBufs;

    /// Pointers to the global thread local variable `__log_buffer` of all
    /// threads that are participating in the current epoch. When a thread
    /// exits, its TLS address will become invalid and must be removed from
    /// this set (that's why we need to keep a separate allocatedBufs).
    std::unordered_set<EventBuffer**> tlsBufAddrs;

    // TODO: how to set this? std::thread::hardware_concurrency()? How to
    // avoid #AppThreads+#Workers > cores? How to dynamically adjust #workers?
    // How to avoid meaningless thread migrations?
    static const int MAX_WORKERS = 32;
};

#endif //FASTLOG_BUFFERMANAGER_H
