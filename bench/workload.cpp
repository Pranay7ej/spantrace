// Sample workload for measuring tracing overhead. Three phases with very
// different call shapes:
//   fib       - millions of tiny recursive calls (worst case for any tracer)
//   pipeline  - a packet parse/checksum loop with a handful of real functions
//   sort      - std::sort with a comparator that the optimizer inlines away
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#define NOINLINE __attribute__((noinline))

NOINLINE int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

namespace pipeline {

struct Packet {
  uint32_t stream;
  uint32_t size;
  int64_t pts;
  const uint8_t* data;
};

NOINLINE bool ParseHeader(const uint8_t* p, size_t avail, Packet* out) {
  if (avail < 16) return false;
  std::memcpy(&out->stream, p, 4);
  std::memcpy(&out->size, p + 4, 4);
  std::memcpy(&out->pts, p + 8, 8);
  out->size %= 4096;
  out->data = p + 16;
  return out->size + 16 <= avail;
}

NOINLINE uint32_t Checksum(const uint8_t* p, size_t n) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < n; ++i) {
    a = (a + p[i]) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

NOINLINE uint64_t Demux(const std::vector<uint8_t>& buf) {
  uint64_t acc = 0;
  size_t off = 0;
  Packet pkt;
  while (ParseHeader(buf.data() + off, buf.size() - off, &pkt)) {
    acc += Checksum(pkt.data, pkt.size) ^ uint64_t(pkt.pts);
    off += 16 + pkt.size;
  }
  return acc;
}

NOINLINE uint64_t Run(int rounds) {
  std::mt19937 rng(42);
  std::vector<uint8_t> buf(4 << 20);
  for (auto& b : buf) b = uint8_t(rng());
  uint64_t acc = 0;
  for (int r = 0; r < rounds; ++r) acc += Demux(buf);
  return acc;
}

}  // namespace pipeline

NOINLINE uint64_t SortPhase() {
  std::mt19937 rng(7);
  std::vector<uint32_t> v(2'000'000);
  for (auto& x : v) x = rng();
  std::sort(v.begin(), v.end(), [](uint32_t a, uint32_t b) { return (a ^ 0x5555) < (b ^ 0x5555); });
  return v[v.size() / 2];
}

template <class Fn>
double Time(const char* name, Fn fn) {
  auto t0 = std::chrono::steady_clock::now();
  volatile auto sink = fn();
  (void)sink;
  double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  std::printf("%s %.1f\n", name, ms);
  return ms;
}

int main() {
  double total = 0;
  total += Time("fib", [] { return fib(34); });
  // Pipeline on two threads so the trace has more than one track.
  total += Time("pipeline", [] {
    uint64_t a = 0, b = 0;
    std::thread t([&] { a = pipeline::Run(6); });
    b = pipeline::Run(6);
    t.join();
    return a + b;
  });
  total += Time("sort", [] { return SortPhase(); });
  std::printf("total %.1f\n", total);
  return 0;
}
