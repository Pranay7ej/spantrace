# spantrace

Compile a C/C++ program with a compiler plugin, run it, and get a timeline of
every function call on every thread that opens straight in
[ui.perfetto.dev](https://ui.perfetto.dev).

I wanted something between "sprinkle `TRACE_EVENT` macros everywhere" and a
sampling profiler: zero source changes, exact call boundaries, and a timeline
view instead of a flame graph. `-finstrument-functions` gets you halfway, but
it instruments before inlining (so you pay for functions that no longer exist)
and hands you raw addresses. This does the instrumentation as an LLVM pass after
the optimizer runs and keeps a name table in the binary.

```sh
clang++ -O2 -fpass-plugin=build/pass/SpanTracePass.so app.cpp build/runtime/libspantrace.a -o app
SPANTRACE_OUT=app.json ./app
# open app.json in ui.perfetto.dev, or:
tools/top.py app.json
```

## Pieces

**The pass** (`pass/SpanTracePass.cpp`, new pass manager plugin, LLVM 18)

- Puts `__st_enter(id)` at the top of each function (after the allocas) and
  `__st_exit(id)` before every `ret`. For `musttail` calls the exit goes in
  front of the call, since nothing is allowed between it and the `ret`.
- Ids are FNV-1a hashes of the function name, so every translation unit agrees
  without coordination. `static` functions also mix in the source file name.
- Each module gets a `{id, name}` table and a constructor that registers it.
  Names are demangled when the trace is written, not at startup.
- `-mllvm -spantrace-filter=<regex>` only instruments functions whose
  *demangled* name matches, e.g. `'^media::'`.
- `-mllvm -spantrace-min-size=N` skips functions with fewer than N IR
  instructions (getters, trivial wrappers).
- Clang parses `-mllvm` flags before `-fpass-plugin` loads anything, so to
  use the options also pass `-Xclang -load -Xclang SpanTracePass.so`.
- Works with `opt -passes=spantrace` too, which is how the tests drive it.

**The runtime** (`runtime/spantrace.cc`)

- Every thread gets its own fixed-size ring buffer the first time it records
  something; that's the only time a lock is taken. After that, recording an
  event is a clock read and a couple of stores into memory only that thread
  writes.
- When a ring fills up it overwrites the oldest events and counts the drops
  (the JSON and `top.py` both tell you). Size it with `SPANTRACE_BUF_EVENTS`
  (default 262144 events, 4 MB per thread).
- The dumper can copy a ring while its thread is still writing. The writer
  bumps a "reserved" counter before touching a slot and a "committed" one after,
  so the reader knows which slots might have been overwritten mid-copy and
  throws those away. There's a test that hammers exactly this.
- Timestamps are `CLOCK_MONOTONIC`.
- The trace is written at exit, or whenever the process gets `SIGUSR1`. The
  signal handler only does a `sem_post`; a background thread does the actual
  writing. (`SPANTRACE_SIGNAL=0` if your app already uses `SIGUSR1`.)
- Output is Chrome trace JSON with complete (`X`) events, one track per thread.
  If exits went missing (exception, longjmp), the writer closes the skipped
  frames at the next matching exit instead of producing garbage nesting.

**tools/top.py** prints the top functions by self or total time. Recursive
functions are counted once per outermost call for total time.

```
$ tools/top.py smoke.json
3 thread(s): json_smoke (1975), json_smoke (1977), json_smoke (1978)
function                          calls    total ms     self ms
main                                  1      13.688      13.688
player::Decoder::decode(int)         40       9.924       9.924
render_frame                         40       4.022       4.022
player::Loop::tick()                 40      13.960       0.014
```

(That's the trace from `tests/json_smoke.cc`. `main` has all self time because
its "children" ran on the other two threads.)

## Build

Needs LLVM 18 dev files for the pass (`llvm-18-dev`, plus `libpolly-18-dev`
on Ubuntu or LLVM's CMake config refuses to load). The runtime builds without
LLVM.

```sh
cmake -S . -B build -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Tests

- runtime: ordering, ring wraparound and drop counting, one ring per thread
  under 4 threads, snapshots taken while another thread is writing flat out,
  demangling, JSON shape and escaping, repairing skipped exits, SIGUSR1 dumps
- a hand-written `.ll` shaped exactly like the pass output, compiled with clang
  and run against the runtime (checks the ABI without needing the plugin)
- the pass: FileCheck tests on small `.ll` files for placement, filtering (on
  demangled names), min size, musttail, allocas and the id scheme

## Overhead

`bench/overhead.sh` builds `bench/workload.cpp` three ways (plain, everything
traced, only `pipeline::*` traced) and reports the median of 5 runs per phase:

- `fib` — millions of tiny recursive calls, the worst case for any tracer
- `pipeline` — a packet parse/checksum loop with a few real functions, 2 threads
- `sort` — `std::sort` with a lambda comparator that gets inlined away

CI runs it on every push and puts the table in the job summary (`full` job),
along with `top.py` output for the traced run.

## Known limits

- Exceptions and `longjmp` skip `__st_exit`. The JSON writer patches the nesting
  back up, but the skipped frames end at the wrong time.
- Functions that got inlined don't show up at all. That's on purpose (it's the
  point of running after the optimizer), but it can surprise you at `-O2`.
- Calls into uninstrumented code (libc, prebuilt libraries) are invisible.
- The runtime uses `initial-exec` TLS, so link it statically into the
  executable rather than `dlopen`ing it.
- Linux only, no Windows support.
