# Exact caller-thread block activity

This is an experimental, opt-in metrics probe. It measures native block
allocation/free activity, not live memory consumption, and does not depend on
allocation sampling.

## Build and query

Enable the counters explicitly:

```console
cmake -S . -B out-thread-activity -DCMAKE_BUILD_TYPE=Release -DMI_THREAD_STATS=ON
cmake --build out-thread-activity --config Release
```

`MI_THREAD_STATS` defaults to `OFF`. It works independently of `MI_STATS` and
`MI_PROFILE`, including when both are disabled.

The probe is declared in `mimalloc-stats.h`:

```c
int mi_thread_activity_get(uint64_t* allocated_bytes, uint64_t* freed_bytes);
int mi_thread_activity_get_and_reset(uint64_t* allocated_bytes, uint64_t* freed_bytes);
```

It reads the calling thread's counters. Call `mi_thread_init()` on each worker
before measured work, including workers that only free objects allocated by
other threads. The getter does not initialize a native thread or collect pages.
It uses the existing TLS access path; it is not an async-signal-safe API.

| Result | Meaning |
| --- | --- |
| `0` | Both outputs contain a successful snapshot. |
| `ENOSYS` | This library was built without thread activity instrumentation. |
| `EAGAIN` | The calling thread has no initialized mimalloc thread context. |
| `EOVERFLOW` | At least one counter has saturated at `UINT64_MAX`. |
| `EINVAL` | At least one output pointer is null. |

This is an errno-style **return value**, not a Boolean and not a change to
`errno`. The getter invokes no error callback. Outputs are unchanged on failure;
callers must not publish stale or uninitialized outputs as a fresh observation.

## Explicit interval consumption

`mi_thread_activity_get` is non-destructive. The separate
`mi_thread_activity_get_and_reset` returns the current interval and starts a new
one by clearing both counters. Both operations are owner-thread-only; no lock,
cross-thread exchange, or wider arithmetic is added to allocation/free.

Use one collecting owner for get-and-reset. Independent reset consumers would
split the same activity between themselves. Non-destructive reads after a reset
observe the new interval, not an independent lifetime cumulative total.

On success, get-and-reset returns exact interval values and clears both counters.
If either counter has saturated, it returns `EOVERFLOW`, leaves both output
values unchanged, and still clears both counters. The caller must report that
interval as lost/incomplete; the next interval can then proceed normally.
`EINVAL`, `EAGAIN`, and `ENOSYS` do not consume or reset anything.

The metrics SDK owns accumulation and export temporality. Feed consumed deltas
to an appropriate delta/`Add` path; do not pass them directly as lifetime totals
to a cumulative observable counter. Retain a consumed interval until the SDK
accepts it, or surface publication loss: another native read will not replay it.
This keeps lifetime accumulation and rollover policy out of mimalloc.

## Exact byte basis and population

Each successful native block allocation contributes `mi_page_block_size(page)`.
Its explicit block free contributes the same full block size to the executing
thread, whether that thread allocated it or not.

This includes size-class rounding and in-block alignment, padding, profiler
metadata, and guard-space overhead. It is not requested size, application-facing
usable size, reserved arena bytes, or RSS. Zero-size requests still consume and
count a native block.

The population is native block operations while a calling thread context is
available, including allocator-internal block operations. Bootstrap operations
before that context is available are outside the observation window. This is
not yet a claim of identical accounting population to jemalloc's thread counters.

In-place resize is not a new block allocation/free. A moving realloc performs
the corresponding native block operations. A failed application request is not
charged as a successful block, although any actual internal block operations
performed while handling a request belong to the measured activity.

Bulk heap destruction is not expanded into synthetic per-block free calls.
Inventory changes caused by that operation belong to the native heap-inventory
view, not to this explicit block-operation counter.

Counters belong to a native thread-data lifetime, with explicit interval resets
available through get-and-reset. They span that thread's heaps
and default-heap switches, and are not cleared by ordinary statistics merges or
`mi_stats_reset()`. Thread teardown ends the observation window; a newly
initialized context starts new counters. Publish final values before teardown,
and use a new observation identity/baseline for a new worker generation.

Counters saturate rather than wrap. `UINT64_MAX` is reserved as an overflow
indication and is never returned as a successful measurement.
Periodic get-and-reset bounds native accumulation to the collection interval;
overflow within an interval remains explicitly detectable and recoverable.

## Caller activity versus origin consumption

For a block of size S allocated by A and freed by B:

```text
A allocation:       A.allocated += S
B explicit free:    B.freed     += S
native collection:  origin heap occupancy decreases
```

Collection does not increment the collector's activity counter again. Thus
`allocated - freed` on one thread is not its live consumption. A can retain a
positive activity balance after B frees the object; B can have more deallocation
activity than allocation activity.

The new counters are updated in the native allocation/free primitives. Native
remote-free propagation and origin accounting remain unchanged. Sampled
reclamation callbacks are not used to estimate these exact activity totals.

The regression scenario holds an anchor block in A's heap, frees a second block
on B, reads B's activity immediately, and then reconciles the native list on A.
It verifies that B receives the free activity, A's activity is not charged
again by collection, and the origin's live block inventory drops to the anchor.
The inventory inspection in that test is quiescent; it does not establish a new
safe arbitrary-thread heap-inventory query.

## Observation and DFE integration

The intended collection pattern is owner-thread sampling followed by the
engine's existing telemetry publication:

```text
worker initializes mimalloc
worker performs allocations/frees
worker's single metrics collector consumes an interval with get-and-reset
DFE's metrics SDK accumulates/publishes those deltas
asynchronous readers consume published telemetry, not another thread's TLD
```

There is no native global thread registry, cross-thread reader, or N-by-N matrix.
Caller-provided output storage is just two `uint64_t` values per observation.
An observer on a different thread cannot use this function to query a worker;
it would read its own context instead.

DFE wiring is not implemented by this patch. Its mimalloc binding must compile
this native source with `MI_THREAD_STATS=1` and expose the getter from the same
allocator instance used by the application. Linking another mimalloc merely to
obtain the getter would produce unrelated counters. The current bundled
`libmimalloc-sys` source/bindings do not automatically provide this new API.

The separate origin-inventory refresh, retirement, and safe publication contract
also remains to be integrated. This probe must not be presented as completing
all of open-telemetry/otel-arrow#3725.

## Cost and validation scope

The opt-in implementation adds two non-atomic 64-bit counters to native
thread-local data. Allocation can use the already known theap's TLD; frees must
find the executing thread's context rather than use the origin page's owner.
Updates include overflow handling. Enabling this option therefore adds real
work to allocation/free and needs workload measurements before production use.

There are no new per-block or per-page fields and no additional free-event queue.
With the option disabled, the counters and update code are compiled out. In the
GCC x64 Release comparison, normalized instruction sequences for `mi_malloc`,
`mi_free`, and both generic free helpers are unchanged.

For the matched GCC 13.3 x64 Release builds with `MI_STATS=OFF`,
`MI_PROFILE=ON`, and `MI_GUARDED=OFF`, symbol sizes illustrate the enabled cost:

| Function | Thread stats OFF | Thread stats ON |
| --- | ---: | ---: |
| `mi_malloc` | 89 bytes | 185 bytes |
| `mi_free` | 197 bytes | 349 bytes |
| `mi_free_generic_local` | 215 bytes | 307 bytes |
| `mi_free_generic_mt` | 199 bytes | 291 bytes |

These sizes include cold branches and are not latency or throughput measurements.
Unlike sampled attribution, exact caller activity needs updates on every block
operation. The option should remain experimental and disabled by default until
representative DFE workloads establish an acceptable cost.

Native tests cover Windows/MSVC, Linux/GCC, feature-disabled availability,
statistics merges/resets, multiple heaps/default-heap switches, remote frees,
native origin reconciliation, sampling coexistence, zero-size/aligned/guarded
blocks, in-place resize, initialization boundaries, interval consumption,
overflow recovery, and ASAN tracking.
These are not a validation of a Rust binding, all TLS models, all architectures,
or production throughput overhead.
