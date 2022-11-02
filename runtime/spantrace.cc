#include "spantrace/spantrace.h"

#include <cxxabi.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <ostream>
#include <thread>
#include <unordered_map>

namespace spantrace {
namespace {

constexpr uint64_t kEnterBit = uint64_t{1} << 63;
constexpr uint64_t kIdMask = ~kEnterBit;

inline uint64_t NowNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

struct Slot {
  std::atomic<uint64_t> ts{0};
  std::atomic<uint64_t> word{0};  // id | kEnterBit for enters
};

// Single-producer ring. Only the owning thread writes; the dumper reads.
//
// The writer bumps `reserved` before touching a slot and `committed` after,
// so a reader can tell which slots it copied might have been overwritten
// mid-copy (a tiny seqlock). Everything is atomics, so it's TSan clean.
struct Ring {
  int tid = 0;
  char name[32] = {};
  size_t mask = 0;
  std::unique_ptr<Slot[]> slots;
  std::atomic<uint64_t> reserved{0};
  std::atomic<uint64_t> committed{0};

  explicit Ring(size_t cap) : mask(cap - 1), slots(new Slot[cap]) {}

  inline void Push(uint64_t word) {
    const uint64_t h = reserved.load(std::memory_order_relaxed);
    reserved.store(h + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    Slot& s = slots[h & mask];
    s.ts.store(NowNs(), std::memory_order_relaxed);
    s.word.store(word, std::memory_order_relaxed);
    committed.store(h + 1, std::memory_order_release);
  }

  ThreadTrace Copy() const {
    const uint64_t cap = mask + 1;
    const uint64_t end = committed.load(std::memory_order_acquire);
    const uint64_t begin = end > cap ? end - cap : 0;
    std::vector<Event> tmp;
    tmp.reserve(end - begin);
    for (uint64_t i = begin; i < end; ++i) {
      const Slot& s = slots[i & mask];
      uint64_t w = s.word.load(std::memory_order_relaxed);
      tmp.push_back({s.ts.load(std::memory_order_relaxed), w & kIdMask, (w & kEnterBit) != 0});
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t res = reserved.load(std::memory_order_relaxed);
    // Anything the writer may have started overwriting while we copied is suspect.
    const uint64_t safe_from = res > cap ? res - cap : 0;
    const size_t skip = safe_from > begin ? size_t(safe_from - begin) : 0;

    ThreadTrace t;
    t.tid = tid;
    t.name = name;
    t.dropped = std::max(begin, safe_from);
    if (skip < tmp.size()) t.events.assign(tmp.begin() + long(skip), tmp.end());
    return t;
  }
};

// Leaked on purpose: module constructors can call __st_register_names before
// this file's statics are initialised, and threads can still be tracing while
// static destructors run at exit.
struct State {
  std::mutex mu;
  std::vector<Ring*> rings;
  std::vector<Ring*> retired;  // see ResetForTesting
  std::atomic<size_t> ring_capacity{0};
  std::mutex names_mu;
  std::unordered_map<uint64_t, const char*> names;
  std::unordered_map<uint64_t, std::string> demangled;
  std::once_flag dumper_once;
  sem_t dump_sem;
  std::atomic<bool> dumped_at_exit{false};
  std::mutex flush_mu;  // SIGUSR1 dump and exit dump can overlap
};

State& G() {
  static State* s = new State;
  return *s;
}

thread_local Ring* t_ring __attribute__((tls_model("initial-exec"))) = nullptr;
thread_local bool t_busy __attribute__((tls_model("initial-exec"))) = false;

size_t RoundPow2(size_t n) {
  size_t p = 16;
  while (p < n) p <<= 1;
  return p;
}

size_t DefaultCapacity() {
  if (const char* e = std::getenv("SPANTRACE_BUF_EVENTS")) {
    long v = std::atol(e);
    if (v > 0) return size_t(v);
  }
  return 262144;
}

const char* OutPath() {
  const char* p = std::getenv("SPANTRACE_OUT");
  return p && *p ? p : "spantrace.json";
}

void DumperMain() {
  State& g = G();
  for (;;) {
    while (sem_wait(&g.dump_sem) != 0) {
    }
    spantrace_flush(OutPath());
  }
}

void SigHandler(int) {
  // Only async-signal-safe work here; the dumper thread does the writing.
  sem_post(&G().dump_sem);
}

void StartDumper() {
  State& g = G();
  sem_init(&g.dump_sem, 0, 0);
  const char* sig = std::getenv("SPANTRACE_SIGNAL");
  if (sig && std::strcmp(sig, "0") == 0) return;
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_handler = SigHandler;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGUSR1, &sa, nullptr);
  // Block SIGUSR1 in the dumper itself so the handler never runs on it.
  sigset_t set, old;
  sigemptyset(&set);
  sigaddset(&set, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &set, &old);
  std::thread(DumperMain).detach();
  pthread_sigmask(SIG_SETMASK, &old, nullptr);
}

Ring* RegisterThread() {
  if (t_busy) return nullptr;  // re-entered from inside our own setup
  t_busy = true;
  State& g = G();
  std::call_once(g.dumper_once, StartDumper);
  size_t cap = g.ring_capacity.load();
  if (!cap) cap = DefaultCapacity();
  auto* r = new Ring(RoundPow2(cap));
  r->tid = int(syscall(SYS_gettid));
  pthread_getname_np(pthread_self(), r->name, sizeof r->name);
  {
    std::lock_guard<std::mutex> lk(g.mu);
    g.rings.push_back(r);
  }
  t_ring = r;
  t_busy = false;
  return r;
}

inline void Record(uint64_t word) {
  Ring* r = t_ring;
  if (__builtin_expect(r == nullptr, 0)) {
    r = RegisterThread();
    if (!r) return;
  }
  r->Push(word);
}

void JsonString(std::ostream& out, const std::string& s) {
  out << '"';
  for (char c : s) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\t': out << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out << buf;
        } else {
          out << c;
        }
    }
  }
  out << '"';
}

void Micros(std::ostream& out, uint64_t ns) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%" PRIu64 ".%03" PRIu64, ns / 1000, ns % 1000);
  out << buf;
}

std::string ProcessName() {
  char buf[64] = {};
  if (FILE* f = std::fopen("/proc/self/comm", "r")) {
    if (std::fgets(buf, sizeof buf, f)) buf[std::strcspn(buf, "\n")] = 0;
    std::fclose(f);
  }
  return buf[0] ? buf : "process";
}

__attribute__((destructor)) void DumpAtExit() {
  State& g = G();
  bool any;
  {
    std::lock_guard<std::mutex> lk(g.mu);
    any = !g.rings.empty();
  }
  if (!any || g.dumped_at_exit.exchange(true)) return;
  if (spantrace_flush(OutPath()) == 0)
    std::fprintf(stderr, "spantrace: wrote %s\n", OutPath());
}

}  // namespace

std::string NameOf(uint64_t id) {
  State& g = G();
  id &= kIdMask;
  std::lock_guard<std::mutex> lk(g.names_mu);
  auto cached = g.demangled.find(id);
  if (cached != g.demangled.end()) return cached->second;
  std::string out;
  auto it = g.names.find(id);
  if (it == g.names.end()) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "fn_%016" PRIx64, id);
    out = buf;
  } else {
    int status = 0;
    char* d = abi::__cxa_demangle(it->second, nullptr, nullptr, &status);
    out = (status == 0 && d) ? d : it->second;
    std::free(d);
  }
  g.demangled.emplace(id, out);
  return out;
}

std::vector<ThreadTrace> Snapshot() {
  State& g = G();
  std::vector<Ring*> rings;
  {
    std::lock_guard<std::mutex> lk(g.mu);
    rings = g.rings;
  }
  std::vector<ThreadTrace> out;
  out.reserve(rings.size());
  for (Ring* r : rings) out.push_back(r->Copy());
  return out;
}

void WriteJson(std::ostream& out, const std::vector<ThreadTrace>& threads) {
  uint64_t t0 = UINT64_MAX, t_end = 0, dropped = 0;
  for (const auto& t : threads) {
    dropped += t.dropped;
    if (!t.events.empty()) {
      t0 = std::min(t0, t.events.front().ts_ns);
      t_end = std::max(t_end, t.events.back().ts_ns);
    }
  }
  if (t0 == UINT64_MAX) t0 = 0;
  const int pid = int(getpid());

  out << "{\"displayTimeUnit\":\"ns\",\"otherData\":{\"clock\":\"CLOCK_MONOTONIC\","
      << "\"dropped_events\":" << dropped << "},\n\"traceEvents\":[\n";
  out << "{\"ph\":\"M\",\"name\":\"process_name\",\"pid\":" << pid << ",\"tid\":0,\"args\":{\"name\":";
  JsonString(out, ProcessName());
  out << "}}";

  struct Open {
    uint64_t id;
    uint64_t ts;
  };
  for (const auto& t : threads) {
    std::string tname = t.name.empty() ? "thread" : t.name;
    out << ",\n{\"ph\":\"M\",\"name\":\"thread_name\",\"pid\":" << pid << ",\"tid\":" << t.tid
        << ",\"args\":{\"name\":";
    JsonString(out, tname + " (" + std::to_string(t.tid) + ")");
    out << ",\"dropped\":" << t.dropped << "}}";

    auto emit = [&](uint64_t id, uint64_t start, uint64_t end, bool unfinished) {
      out << ",\n{\"ph\":\"X\",\"pid\":" << pid << ",\"tid\":" << t.tid << ",\"ts\":";
      Micros(out, start - t0);
      out << ",\"dur\":";
      Micros(out, end - start);
      out << ",\"name\":";
      JsonString(out, NameOf(id));
      if (unfinished) out << ",\"args\":{\"unfinished\":true}";
      out << "}";
    };

    std::vector<Open> stack;
    for (const Event& e : t.events) {
      if (e.enter) {
        stack.push_back({e.id, e.ts_ns});
        continue;
      }
      // Exits normally match the top. If they don't, something (an exception,
      // longjmp) skipped exits: close frames down to the match. An exit with no
      // matching enter at all started before the ring's oldest event; drop it.
      auto match = std::find_if(stack.rbegin(), stack.rend(),
                                [&](const Open& o) { return o.id == e.id; });
      if (match == stack.rend()) continue;
      while (!stack.empty()) {
        Open o = stack.back();
        stack.pop_back();
        emit(o.id, o.ts, e.ts_ns, false);
        if (o.id == e.id) break;
      }
    }
    while (!stack.empty()) {  // still running when we dumped
      Open o = stack.back();
      stack.pop_back();
      emit(o.id, o.ts, t_end, true);
    }
  }
  out << "\n]}\n";
}

void SetRingCapacity(size_t events) { G().ring_capacity.store(events); }

void ResetForTesting() {
  State& g = G();
  std::lock_guard<std::mutex> lk(g.mu);
  // Rings are parked rather than freed: a live thread could still hold one in
  // its t_ring. The current thread's pointer we can clear.
  g.retired.insert(g.retired.end(), g.rings.begin(), g.rings.end());
  g.rings.clear();
  t_ring = nullptr;
}

}  // namespace spantrace

extern "C" {

void __st_enter(uint64_t id) { spantrace::Record((id & spantrace::kIdMask) | spantrace::kEnterBit); }

void __st_exit(uint64_t id) { spantrace::Record(id & spantrace::kIdMask); }

void __st_register_names(const st_name_entry* table, uint64_t count) {
  auto& g = spantrace::G();
  std::lock_guard<std::mutex> lk(g.names_mu);
  for (uint64_t i = 0; i < count; ++i) g.names.emplace(table[i].id & spantrace::kIdMask, table[i].name);
}

int spantrace_flush(const char* path) {
  std::lock_guard<std::mutex> lk(spantrace::G().flush_mu);
  const std::string tmp = std::string(path) + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) return -1;
    spantrace::WriteJson(out, spantrace::Snapshot());
    if (!out) return -1;
  }
  return std::rename(tmp.c_str(), path) == 0 ? 0 : -1;
}

}  // extern "C"
