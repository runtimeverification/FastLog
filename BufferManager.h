#ifndef FASTLOG_BUFFERMANAGER_H
#define FASTLOG_BUFFERMANAGER_H

#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_set>

#include "EventBuffer.h"
#include "Utils.h"

class BufferManager {
  public:
    explicit BufferManager()
        : epoch(0)
        , epochMutex()
        , freeBufs()
        , reclaimedBufs()
        , tlsBufAddrs()
    {}

    void allocBuffer();
    void release(std::vector<EventBuffer*>* bufsToRelease);
    bool tryIncEpoch(EventBuffer::Ref* ref);
    void threadExit();

  private:
    EventBuffer* getFreshBuf();

    /// Current epoch number.
    std::atomic<int> epoch;

    // TODO: is std::mutex too slow? use SpinLock instead?
    std::mutex epochMutex;

    /// Event buffers that are currently available.
    std::vector<EventBuffer*> freeBufs;

    /// Event buffers allocated in the previous epoch.
    std::vector<EventBuffer*> reclaimedBufs;

    /// Pointers to the global thread local variable `__log_buffer` of all
    /// threads that are participating in the current epoch.
    std::unordered_set<EventBuffer**> tlsBufAddrs;
};

#endif //FASTLOG_BUFFERMANAGER_H
