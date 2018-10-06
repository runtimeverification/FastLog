#ifndef FASTLOG_WORKER_H
#define FASTLOG_WORKER_H

#include <vector>
#include "BufferManager.h"

static void
workerMain(BufferManager* bufferManager, std::vector<EventBuffer*> buffers)
{
    // Wait until all buffers are safe to read.
    for (auto buf : buffers) {
        while (!buf->closed) {}
    }

    // TODO: dummy workers simply count the number of events.
    int events = 0;
    for (auto buf : buffers) {
        events += buf->events;
    }
    printf("Worker thread processed %d events\n", events);

    // Return buffers back to the manager.
    bufferManager->release(&buffers);
}

#endif //FASTLOG_WORKER_H
