// Emits a small multi-threaded trace through the public runtime API and writes
// it to argv[1]. CTest then validates it with python's json module and top.py.
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "spantrace/spantrace.h"

static const st_name_entry kNames[] = {
    {1, "main"},
    {2, "_ZN6player4Loop4tickEv"},       // player::Loop::tick()
    {3, "_ZN6player7Decoder6decodeEi"},  // player::Decoder::decode(int)
    {4, "render_frame"},
};

static void Busy(int us) {
  auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
  while (std::chrono::steady_clock::now() < end) {
  }
}

static void Tick(int i) {
  __st_enter(2);
  __st_enter(3);
  Busy(200 + i % 3 * 50);
  __st_exit(3);
  __st_enter(4);
  Busy(100);
  __st_exit(4);
  __st_exit(2);
}

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  __st_register_names(kNames, 4);
  __st_enter(1);
  std::vector<std::thread> threads;
  for (int t = 0; t < 2; ++t)
    threads.emplace_back([] {
      for (int i = 0; i < 20; ++i) Tick(i);
    });
  for (auto& t : threads) t.join();
  __st_exit(1);
  return spantrace_flush(argv[1]) == 0 ? 0 : 1;
}
