// libspantrace: the runtime half of spantrace.
//
// The compiler pass emits calls to __st_enter / __st_exit and registers a name
// table per module. Everything else here (snapshots, JSON writer) is for the
// dumper and for tests.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct st_name_entry {
  uint64_t id;
  const char* name;  // mangled; demangled when the trace is written
};

void __st_enter(uint64_t id);
void __st_exit(uint64_t id);
void __st_register_names(const struct st_name_entry* table, uint64_t count);

// Write the trace now (same as SIGUSR1, but synchronous). Returns 0 on success.
int spantrace_flush(const char* path);

#ifdef __cplusplus
}  // extern "C"

#include <iosfwd>
#include <string>
#include <vector>

namespace spantrace {

struct Event {
  uint64_t ts_ns;
  uint64_t id;
  bool enter;
};

struct ThreadTrace {
  int tid;
  std::string name;
  uint64_t dropped;           // events overwritten because the ring was full
  std::vector<Event> events;  // oldest first
};

// Consistent copy of every thread's ring. Safe while other threads keep tracing.
std::vector<ThreadTrace> Snapshot();

// Chrome / Perfetto trace JSON ("X" complete events, one track per thread).
void WriteJson(std::ostream& out, const std::vector<ThreadTrace>& threads);

// Demangled name for an id, or "fn_<hex id>" if it was never registered.
std::string NameOf(uint64_t id);

// Events per thread ring. Only affects threads that haven't traced yet.
// Defaults to SPANTRACE_BUF_EVENTS or 262144.
void SetRingCapacity(size_t events);

// Test helper: forget all threads and events. Callers must make sure no other
// thread is tracing.
void ResetForTesting();

}  // namespace spantrace
#endif
