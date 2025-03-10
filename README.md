# spantrace

Compile a C/C++ program with this LLVM plugin, run it, and you get a trace of every function call on every thread that you can open in [ui.perfetto.dev](https://ui.perfetto.dev).

I wanted something like `-finstrument-functions` but that runs after the optimizer (so inlined stuff isn't traced) and gives you real function names instead of addresses.

```
clang++ -O2 -fpass-plugin=build/pass/SpanTracePass.so app.cpp build/runtime/libspantrace.a -o app
SPANTRACE_OUT=app.json ./app
tools/top.py app.json
```

## how it works

**pass** (LLVM 18, new pass manager): adds `__st_enter(id)` at the start of each function and `__st_exit(id)` before every return. ids are hashes of the function name so every file agrees. each module also registers a table of id -> name at startup.

options:
- `-mllvm -spantrace-filter=<regex>` only trace matching functions (matches demangled names, e.g. `'^media::'`)
- `-mllvm -spantrace-min-size=N` skip tiny functions

annoying gotcha: clang parses `-mllvm` flags before `-fpass-plugin` loads the plugin, so if you use the options you also need `-Xclang -load -Xclang SpanTracePass.so`.

**runtime**: each thread gets its own ring buffer, so recording is lock-free (the only lock is when a thread registers the first time). when the ring is full it overwrites the oldest events and counts the drops. size is `SPANTRACE_BUF_EVENTS`, default 262144.

trace gets written at exit or on `SIGUSR1`. the signal handler just posts a semaphore and a background thread does the writing.

**tools/top.py**: top functions by self/total time.

## build

needs llvm-18-dev (and libpolly-18-dev on ubuntu or LLVM's cmake config won't load). the runtime builds without LLVM.

```
cmake -S . -B build -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build -j
ctest --test-dir build
```

tests cover the runtime (wraparound, multiple threads, reading while another thread writes, json output, sigusr1) and the pass itself with FileCheck on small .ll files.

## overhead

`bench/overhead.sh` builds a small workload 3 ways. numbers from CI (github ubuntu runner, clang 18, -O2):

| phase | plain ms | traced ms | slowdown | filtered ms |
|---|---:|---:|---:|---:|
| fib | 12.4 | 400.2 | 32x | 10.6 |
| pipeline | 60.2 | 235.0 | 3.9x | 65.8 |
| sort | 129.5 | 213.0 | 1.6x | 118.8 |
| total | 202.1 | 851.3 | 4.2x | 195.3 |

fib is the worst case since the function does basically nothing, so the tracing is most of the cost. with a filter the overhead is basically gone. sort is cheap because the comparator got inlined before the pass ran.

also the full trace was 94 MB and still dropped a lot of events, so for hot code either use a filter or a bigger buffer.

## tracing lsmkv

CI also builds my [lsmkv](https://github.com/Pranay7ej/lsmkv) storage engine with the plugin (filter `^lsmkv::`) and runs its benchmark on 100k keys. top of the background thread:

```
function                                                   calls    total ms     self ms
lsmkv::DBImpl::BackgroundLoop()                                1     351.649     195.805
lsmkv::WritableFile::Sync()                                    9      82.755      82.755
lsmkv::TableBuilder::WriteRawBlock(...)                     2719      35.046      30.327
lsmkv::(anon)::TwoLevelIterator::key() const              528266      39.623      26.963
lsmkv::(anon)::MergingIterator::FindSmallest()             83837      93.858      20.951
lsmkv::DBImpl::WriteLevel0Table(...)                           3     144.457      16.936
```

two things I didn't expect: fsync is a quarter of the background thread's time, and a big chunk of compaction is just `key()` calls going through three layers of iterators (merging -> level -> two-level -> block), half a million of them. so the tracing overhead is inflating those, but it does point at the virtual call chain as the thing to flatten.

it also dropped ~63M events on the main thread even with a 4M event buffer, which is why the foreground side is missing from the table. tracing everything in a hot loop just produces too much.

## limitations
- exceptions and longjmp skip the exit call. the json writer tries to fix up the nesting but the timing on those is off
- inlined functions don't show up (on purpose)
- runtime uses initial-exec TLS so link it statically
- linux only
