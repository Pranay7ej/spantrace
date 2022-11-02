#include <gtest/gtest.h>

#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "spantrace/spantrace.h"

using spantrace::Snapshot;
using spantrace::ThreadTrace;

namespace {

class Runtime : public ::testing::Test {
 protected:
  void SetUp() override {
    spantrace::ResetForTesting();
    spantrace::SetRingCapacity(1 << 12);
  }
  void TearDown() override { spantrace::ResetForTesting(); }

  // Runs fn on a fresh thread (so it gets a fresh ring) and returns that
  // thread's trace.
  template <class Fn>
  ThreadTrace OnThread(Fn fn) {
    std::atomic<int> tid{0};
    std::thread([&] {
      fn();
      tid = int(syscall(SYS_gettid));
    }).join();
    for (auto& t : Snapshot())
      if (t.tid == tid) return t;
    return {};
  }

  static size_t Count(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) ++n;
    return n;
  }
};

}  // namespace

TEST_F(Runtime, RecordsEnterExitInOrder) {
  auto t = OnThread([] {
    __st_enter(1);
    __st_enter(2);
    __st_exit(2);
    __st_exit(1);
  });
  ASSERT_EQ(t.events.size(), 4u);
  EXPECT_EQ(t.dropped, 0u);
  EXPECT_TRUE(t.events[0].enter);
  EXPECT_EQ(t.events[0].id, 1u);
  EXPECT_TRUE(t.events[1].enter);
  EXPECT_EQ(t.events[1].id, 2u);
  EXPECT_FALSE(t.events[2].enter);
  EXPECT_FALSE(t.events[3].enter);
  for (size_t i = 1; i < t.events.size(); ++i)
    EXPECT_LE(t.events[i - 1].ts_ns, t.events[i].ts_ns);
}

TEST_F(Runtime, RingOverwritesOldestAndCountsDrops) {
  spantrace::SetRingCapacity(16);
  auto t = OnThread([] {
    for (uint64_t i = 0; i < 20; ++i) {
      __st_enter(100 + i);
      __st_exit(100 + i);
    }
  });
  ASSERT_EQ(t.events.size(), 16u);
  EXPECT_EQ(t.dropped, 24u);
  // Survivors are the newest 8 pairs: 112..119.
  for (size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(t.events[i].id, 112 + i / 2) << i;
    EXPECT_EQ(t.events[i].enter, i % 2 == 0) << i;
  }
}

TEST_F(Runtime, CapacityRoundsUpToPowerOfTwo) {
  spantrace::SetRingCapacity(20);  // -> 32
  auto t = OnThread([] {
    for (int i = 0; i < 50; ++i) __st_enter(7);
  });
  EXPECT_EQ(t.events.size(), 32u);
  EXPECT_EQ(t.dropped, 18u);
}

TEST_F(Runtime, EachThreadGetsItsOwnRing) {
  constexpr int kThreads = 4, kPairs = 1000;
  std::vector<std::thread> threads;
  for (int k = 0; k < kThreads; ++k) {
    threads.emplace_back([k] {
      for (int i = 0; i < kPairs; ++i) {
        __st_enter(uint64_t(k) * 10000 + uint64_t(i));
        __st_exit(uint64_t(k) * 10000 + uint64_t(i));
      }
    });
  }
  for (auto& th : threads) th.join();

  auto snap = Snapshot();
  ASSERT_EQ(snap.size(), size_t(kThreads));
  std::vector<int> tids;
  for (auto& t : snap) {
    tids.push_back(t.tid);
    ASSERT_EQ(t.events.size(), size_t(2 * kPairs));
    const uint64_t base = t.events[0].id / 10000 * 10000;
    for (size_t i = 0; i < t.events.size(); ++i) {
      // No events from another thread leaked into this ring.
      ASSERT_EQ(t.events[i].id, base + i / 2);
      if (i) {
        ASSERT_LE(t.events[i - 1].ts_ns, t.events[i].ts_ns);
      }
    }
  }
  std::sort(tids.begin(), tids.end());
  EXPECT_EQ(std::unique(tids.begin(), tids.end()), tids.end());
}

TEST_F(Runtime, SnapshotWhileWritingIsConsistent) {
  spantrace::SetRingCapacity(64);
  std::atomic<bool> stop{false}, started{false};
  std::thread writer([&] {
    uint64_t i = 0;
    __st_enter(0);
    started = true;
    while (!stop.load(std::memory_order_relaxed)) {
      __st_exit(i);
      __st_enter(++i);
    }
  });
  while (!started) std::this_thread::yield();
  for (int round = 0; round < 2000; ++round) {
    for (auto& t : Snapshot()) {
      // The writer emits exit(i), enter(i+1), exit(i+1)... so any torn slot
      // would break this chain.
      for (size_t k = 1; k < t.events.size(); ++k) {
        const auto& a = t.events[k - 1];
        const auto& b = t.events[k];
        ASSERT_NE(a.enter, b.enter) << "round " << round;
        ASSERT_EQ(b.id, a.enter ? a.id : a.id + 1) << "round " << round;
        ASSERT_LE(a.ts_ns, b.ts_ns);
      }
    }
  }
  stop = true;
  writer.join();
}

TEST_F(Runtime, NamesAreDemangled) {
  static const st_name_entry table[] = {
      {0x1111, "_Z3fooi"},
      {0x2222, "main"},
      {0x3333, "_ZN2ns6Widget4drawEv"},
  };
  __st_register_names(table, 3);
  EXPECT_EQ(spantrace::NameOf(0x1111), "foo(int)");
  EXPECT_EQ(spantrace::NameOf(0x2222), "main");
  EXPECT_EQ(spantrace::NameOf(0x3333), "ns::Widget::draw()");
  EXPECT_EQ(spantrace::NameOf(0xabc), "fn_0000000000000abc");
}

TEST_F(Runtime, JsonHasOneCompleteEventPerCall) {
  static const st_name_entry table[] = {{11, "outer"}, {12, "inner"}, {13, "after"}};
  __st_register_names(table, 3);
  OnThread([] {
    __st_enter(11);
    __st_enter(12);
    __st_exit(12);
    __st_exit(11);
    __st_enter(13);
    __st_exit(13);
  });
  std::ostringstream out;
  spantrace::WriteJson(out, Snapshot());
  const std::string json = out.str();
  EXPECT_EQ(json.rfind("{\"displayTimeUnit\":\"ns\"", 0), 0u);
  EXPECT_EQ(Count(json, "\"ph\":\"X\""), 3u);
  EXPECT_EQ(Count(json, "\"name\":\"outer\""), 1u);
  EXPECT_EQ(Count(json, "\"name\":\"inner\""), 1u);
  EXPECT_EQ(Count(json, "\"name\":\"after\""), 1u);
  EXPECT_EQ(Count(json, "\"thread_name\""), 1u);
  EXPECT_EQ(Count(json, "\"process_name\""), 1u);
  EXPECT_EQ(Count(json, "unfinished"), 0u);
  EXPECT_EQ(Count(json, "{"), Count(json, "}"));
  EXPECT_EQ(Count(json, "["), Count(json, "]"));
}

TEST_F(Runtime, JsonRepairsSkippedExitsAndUnfinishedCalls) {
  OnThread([] {
    __st_exit(50);   // enter happened before the ring's oldest event: dropped
    __st_enter(1);
    __st_enter(2);   // think: exception thrown out of 2, caught in 1
    __st_exit(1);    // closes 2 and 1
    __st_enter(3);   // still running at dump time
  });
  std::ostringstream out;
  spantrace::WriteJson(out, Snapshot());
  const std::string json = out.str();
  EXPECT_EQ(Count(json, "\"ph\":\"X\""), 3u);
  EXPECT_EQ(Count(json, "fn_0000000000000032"), 0u);  // id 50
  EXPECT_EQ(Count(json, "\"unfinished\":true"), 1u);
}

TEST_F(Runtime, JsonEscapesNames) {
  static const st_name_entry table[] = {{77, "weird\"name\\with\ncontrol"}};
  __st_register_names(table, 1);
  OnThread([] {
    __st_enter(77);
    __st_exit(77);
  });
  std::ostringstream out;
  spantrace::WriteJson(out, Snapshot());
  EXPECT_NE(out.str().find("weird\\\"name\\\\with\\ncontrol"), std::string::npos);
}

TEST_F(Runtime, SigusrWritesTraceFromBackgroundThread) {
  char path[] = "/tmp/spantrace_sigXXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);
  unlink(path);
  setenv("SPANTRACE_OUT", path, 1);
  static const st_name_entry table[] = {{4242, "_Z13signal_workerv"}};
  __st_register_names(table, 1);
  OnThread([] {
    __st_enter(4242);
    __st_exit(4242);
  });
  raise(SIGUSR1);
  std::string json;
  for (int i = 0; i < 200 && json.empty(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::ifstream in(path);
    if (in) json.assign(std::istreambuf_iterator<char>(in), {});
  }
  unlink(path);
  unsetenv("SPANTRACE_OUT");
  ASSERT_FALSE(json.empty()) << "no trace written after SIGUSR1";
  EXPECT_NE(json.find("signal_worker()"), std::string::npos);
}
