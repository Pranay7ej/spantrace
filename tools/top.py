#!/usr/bin/env python3
"""Top functions by total and self time from a spantrace JSON file.

    tools/top.py spantrace.json            # top 20 by self time
    tools/top.py spantrace.json -n 40 --sort total
    tools/top.py spantrace.json --tid 12345

Total time counts a recursive function once per outermost call, so fib() doesn't
show up with 30x the program's runtime. Self time is total minus time spent in
traced children.
"""
import argparse
import json
import sys
from collections import defaultdict


def load(path):
    with open(path) as f:
        data = json.load(f)
    events = data["traceEvents"] if isinstance(data, dict) else data
    threads = {}
    by_tid = defaultdict(list)
    for e in events:
        if e.get("ph") == "M" and e.get("name") == "thread_name":
            threads[e["tid"]] = e["args"]["name"]
        elif e.get("ph") == "X":
            by_tid[e["tid"]].append(e)
    dropped = data.get("otherData", {}).get("dropped_events", 0) if isinstance(data, dict) else 0
    return by_tid, threads, dropped


def aggregate(spans):
    """spans: X events for one thread. Returns name -> [calls, total_us, self_us]."""
    stats = defaultdict(lambda: [0, 0.0, 0.0])
    # Parents start earlier; on a tie the longer one is the parent.
    spans = sorted(spans, key=lambda e: (e["ts"], -e["dur"]))
    stack = []  # (end_ts, name, index into child_time, dur)
    child_time = []
    active = defaultdict(int)  # name -> depth on the current stack

    def pop():
        end, name, idx, dur = stack.pop()
        s = stats[name]
        s[2] += dur - child_time[idx]
        active[name] -= 1
        if active[name] == 0:
            s[1] += dur
        if stack:
            child_time[stack[-1][2]] += dur

    for e in spans:
        start, dur, name = e["ts"], e["dur"], e["name"]
        while stack and stack[-1][0] <= start:
            pop()
        stats[name][0] += 1
        active[name] += 1
        child_time.append(0.0)
        stack.append((start + dur, name, len(child_time) - 1, dur))
    while stack:
        pop()
    return stats


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace")
    ap.add_argument("-n", type=int, default=20, help="rows to show")
    ap.add_argument("--sort", choices=["self", "total", "calls"], default="self")
    ap.add_argument("--tid", type=int, help="only this thread")
    args = ap.parse_args(argv)

    by_tid, names, dropped = load(args.trace)
    merged = defaultdict(lambda: [0, 0.0, 0.0])
    for tid, spans in by_tid.items():
        if args.tid is not None and tid != args.tid:
            continue
        for name, (calls, total, self_) in aggregate(spans).items():
            m = merged[name]
            m[0] += calls
            m[1] += total
            m[2] += self_

    if not merged:
        print("no spans found", file=sys.stderr)
        return 1
    key = {"calls": 0, "total": 1, "self": 2}[args.sort]
    rows = sorted(merged.items(), key=lambda kv: kv[1][key], reverse=True)[: args.n]

    shown = [n for t, n in names.items() if args.tid is None or t == args.tid]
    print(f"{len(by_tid)} thread(s): {', '.join(shown)}")
    if dropped:
        print(f"note: {dropped} events were dropped (ring buffer wrapped); oldest calls are missing")
    width = min(60, max(len(n) for n, _ in rows))
    print(f"{'function':<{width}}  {'calls':>9}  {'total ms':>10}  {'self ms':>10}")
    for name, (calls, total, self_) in rows:
        label = name if len(name) <= width else name[: width - 1] + "…"
        print(f"{label:<{width}}  {calls:>9}  {total / 1000:>10.3f}  {self_ / 1000:>10.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
