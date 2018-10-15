#include "Context.h"

__thread EventBuffer* __log_buffer = NULL;
thread_local BufferManager __buf_manager;
thread_local Context __thr_context;
std::atomic<int> Context::threadCounter(0);
std::atomic<uint32_t> __event_id_counter(0);
