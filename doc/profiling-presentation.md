# mimalloc profiling: one small mechanism, multiple consumers

**Presentation draft for discussion with Daan Leijen**

Prepared 2026-09-09. Proposal, not an accepted API or an implemented feature.
Branch observations refer to `dev3-profile` at `eaaed425687b`.
The [technical draft](profiling-sampling-design.md) contains the derivations,
integration details, and implementation acceptance criteria.

---

## 1. The proposal

**Keep the allocator mechanism small; make the profiling choices explicit.**

- Shared sampled-allocation and lifetime hooks serve both external and
  in-process profilers.
- Prefer TCMalloc-style weighting initially; preserve the information needed
  for inclusion-probability weighting over the same selections.
- Build an optional direct-to-file consumer, initially producing unsymbolized
  pprof with sufficient module identity for offline symbolization.
- Do not put unwinders, stack tables, protobuf, compression, or file I/O in
  mimalloc's ordinary allocation path.

**Discussion goal:** agree on the allocator boundary and lifecycle before
choosing every downstream library and output.

---

## 2. The design hierarchy

| Layer | Decision | Proposed owner |
| --- | --- | --- |
| Sampling policy | Charge, distribution, mean, estimator inputs | Allocator sampler and shared statistical contract |
| Hook mechanism | Final pointer/sizes, synchronous notification, retained identity | mimalloc |
| Consumer attachment | Direct callbacks or USDT probe emission | Optional adapter |
| Context capture | Native unwinder, eBPF unwinder, or runtime-supplied frames | Consumer/provider |
| Sample state | Live identities, original contributions, stack identity | Consumer, with allocator lifetime cookies |
| Aggregation | Per-theap shards or external event correlation | Consumer |
| Representation | Requested/usable bytes, objects, cumulative/live profiles | Shared semantics and consumer |
| Output | pprof file, OTLP Profiles, other future formats | Exporter |
| Symbolization | Address-only, partial, or embedded symbols | Consumer or offline tooling |

These layers have contracts, but they are not one inseparable implementation.
For example, supporting OTel's USDT contract does not require mimalloc to
serialize OTLP.

---

## 3. Daan's branch already supplies the foundation

| Present in the inspected branch | Still needs a defined contract or implementation |
| --- | --- |
| Per-theap countdown and RNG state | Randomized intervals, distinct mean and remaining distance |
| Sampled-block tag and consumer storage | Stable originating-profiler ownership through the full lifetime |
| Allocation/free callbacks with requested and usable sizes | Final user pointer and original request across aligned paths |
| Declared in-place realloc callback | Callback wiring and resize accounting |
| Deferred page/theap statistics | Separate sample-state publication and snapshot semantics |
| Heap-scoped profiler registration | Optional per-theap consumer-state lifecycle |

Default sampling uses coarse page accounting; fine-grained allocation
accounting exists separately. Historical page totals cannot recover the stacks
responsible for those allocations.

Source: [hook API][mi-api], [theap layout][mi-layout],
[sampling path][mi-sample], [page statistics][mi-stats].

---

## 4. One selection process can support either estimator

Under the draft's discrete geometric reference model:

```text
R = mean interval       x = charge, initially proposed as requested_size + 1
d = remaining distance at allocation entry

if d <= x: select this allocation; W = R + x - d; draw next interval
p = 1 - (1 - 1/R)^x

TCMalloc-style object weight = W/x       preferred initial estimator
Inclusion object weight     = 1/p       supported alternative

requested contribution = requested_size * object_weight
usable contribution    = usable_size    * object_weight
```

Preserve the selected contribution until free; merge contributions, not raw
sample counts followed by a guessed correction.

**Preference is not an ABI restriction.** `R`, `x`, `W`, and a documented
sampling law support both calculations. Finite-precision generation, tail
handling, rounding, and size meaning still need agreement.

The occasional logarithm is not the primary optimization target. Jemalloc
also caches weights by size class; compare footprint and total cost at
comparable accuracy, not just arithmetic per event.

---

### 4a. First define the experiment, then the estimator

Hold the application's allocation sizes, stacks, and lifetimes fixed.
Randomness comes from the profiler, not the workload.

```text
r = requested bytes         a = usable bytes
x = sampling charge        R = mean distance between sampling points
D = remaining random distance at allocation entry
I = 1 if D <= x, otherwise 0
```

For the exact discrete model, each charge unit has a sampling point with
probability `1/R`; `D` is geometric on `1, 2, ...`.
Memorylessness preserves that residual distribution after unsampled allocations.
After a sampled allocation, draw a fresh distance starting at its end.

```text
p = P(I = 1) = 1 - (1 - 1/R)^x

For any fixed object property y:
    estimated contribution = I * y * object_weight
    require E[I * object_weight] = 1
```

Use `y = 1`, `r`, or `a` to estimate objects, requested bytes, or usable bytes.
The proposed `x = r + 1` lets zero-request allocations participate without
pretending the extra charge unit is an application byte.

---

### 4b. Why both estimators are unbiased

**Inclusion weighting:** a selected object represents `1/p` objects.

```text
E[I * (1/p)] = p * (1/p) = 1
```

**Mean-plus-overshoot:** imagine counting every sampling point in the
allocation. If that count is `N`, then `E[N] = x/R`, so `R*N` estimates `x`.

We do not need to generate all those points. Once the first is found at `d`,
the expected number in the remaining `x-d` charge units is `(x-d)/R`.

```text
W = E[R*N | first point at d] = R + x - d
E[I*W] = x
object_weight = W/x
```

There is a useful hierarchy of conditional averaging:

```text
count every point -> retain first point only -> retain selection only
       R*N                  I*W                     I*x/p
```

Each step preserves the mean while removing some randomness. TCMalloc-style
weighting stops at the middle step; inclusion weighting goes one step further.
These statements concern the reference model, not exact implementation
equivalence between the two allocators.

---

### 4c. Worked example: same object, same selections, different weights

Use a deliberately small exact example: `R = 4`, `x = 2`.
Then `p = 1 - (3/4)^2 = 7/16`.

| Outcome | Probability | TCMalloc-style object contribution | Inclusion object contribution |
| --- | --- | --- | --- |
| First point at `d = 1` | `1/4` | `(4+2-1)/2 = 5/2` | `16/7` |
| First point at `d = 2` | `3/16` | `(4+2-2)/2 = 2` | `16/7` |
| No point in this allocation | `9/16` | `0` | `0` |

```text
TCMalloc-style expectation = (1/4)*(5/2) + (3/16)*2 = 1
Inclusion expectation     = (7/16)*(16/7)           = 1
```

If this object requested one byte and has eight usable bytes, multiply either
object contribution by `1` or `8`. Expected requested bytes are `1`; expected
usable bytes are `8`.

The two reported weights need not agree on a particular sample. Neither is
claiming the program really allocated 2.5 objects: it is an expansion estimate.
The conditional mean of `W/x` among selected objects is exactly `1/p`.

---

### 4d. Accuracy, cost, and what the hook must preserve

For the same discrete selections, compare variance of estimated charge:

```text
V_overshoot = R*(R-1)*p
V_inclusion = x*x*(1-p)/p
```

| Allocation regime | Consequence |
| --- | --- |
| `x << R`, with large `R` | Both variances are approximately `R*x`; little difference. |
| `x` comparable to `R` | Inclusion weighting removes noticeable conditional weight variation. |
| `x >> R` | Selection is almost certain. Inclusion variance tends to zero; overshoot standard deviation tends to `sqrt(R*(R-1))`. |

For a measured property `y`, multiply these variances by `(y/x)^2`.
Overshoot's relative error still decreases for large objects. These are
per-allocation comparisons under a fixed model, not a universal claim about
whole-profile variance under correlated or adaptive sampling.

Both approaches amortize random interval generation. Cached class weights can
make inclusion weighting cheap; overshoot supplies compact integer inputs
without a table for arbitrary request sizes.

**Hook requirement:** retain `R`, `x`, `W` under a documented law. Convert to
contributions before merging and preserve the original contribution through
free. A mean alone does not specify `p`; a final `1/p` weight cannot recover
the realized overshoot.

For continuous exponential sampling use `p = 1-exp(-x/R)`, not the discrete
formula. Production implementations must specify their approximation and
rounding; see the [mathematical detail](profiling-sampling-design.md#estimator-derivation-and-worked-example).

---

## 5. The shared hooks describe real allocation lifetimes

```text
sampled allocation:
    final user pointer + original sizes + selection metadata + consumer cookie

sampled free:
    original allocation identity/cookie, before storage can be reused

in-place resize:
    explicitly defined transition, not an undocumented synthetic allocation

optional local-state lifecycle:
    initialize -> publish -> retire
```

- Notify at the allocating call site, after successful pointer finalization.
- Charge the application request once; exclude internal retries and metadata.
- Cross-thread frees retain the original allocation's attribution and weight.
- Pausing new samples is different from discarding outstanding live state.
- Callback safety and teardown lock context are part of the contract.

These are semantic operations, not a finalized C ABI.
The current heap has one profiler registration; simultaneous consumers would
need explicit dispatch rather than being implied by "shared hooks."

---

### 5a. Proposed allocation and lifetime declarations

Discussion-only API revision; not declarations already implemented by the
branch. `mi_profiler_data_t` retains its requested/usable sizes and consumer
`user_data` region.

```c
typedef struct mi_profiler_sample_s {
  size_t   mean_interval;       /* R */
  size_t   sampling_charge;     /* x */
  uint64_t weighted_charge;     /* W */
} mi_profiler_sample_t;

typedef size_t (mi_cdecl mi_profiler_on_alloc_fun)(
    mi_profiler_data_t* data, void* ptr,
    const mi_profiler_sample_t* sample,
    const mi_heap_t* heap, void* local_arg, void* profiler_arg);

typedef void (mi_cdecl mi_profiler_on_free_fun)(
    mi_profiler_data_t* data, void* ptr, void* profiler_arg);

typedef void (mi_cdecl mi_profiler_on_realloc_inplace_fun)(
    mi_profiler_data_t* data, void* ptr,
    size_t old_requested_size, size_t old_usable_size,
    void* profiler_arg);
```

Allocation returns zero to retain the mean, or a new mean for the next
interval. `sample` is borrowed; the consumer retains its original contribution.
Free/resize use the original record in `data->user_data`, not the freeing
thread's local shard. Resize supplies old sizes alongside the new sizes in
`data`; its OTel representation remains an open decision.

---

### 5b. Proposed local-state declarations

```c
typedef int (mi_cdecl mi_profiler_local_init_fun)(
    const mi_heap_t* heap, void* profiler_arg, void** local_arg);

typedef int (mi_cdecl mi_profiler_local_publish_fun)(
    void* local_arg, void* profiler_arg);

typedef void (mi_cdecl mi_profiler_local_retire_fun)(
    void* local_arg, void* profiler_arg);
```

Add `initial_mean_interval` and these three callback pointers to the profiler
descriptor. Keep the existing heap/subprocess registration functions.

- Init/publish return zero on success or a nonzero error code; failures surface.
- Init creates the opaque per-theap context passed to allocation callbacks.
- Publish transfers pending deltas; it is not a global snapshot barrier.
- Retire transfers ownership after local callbacks quiesce; outstanding remote
  frees can still reference the consumer's retained shard.
- A USDT-only adapter can omit these local hooks and accumulate externally.

Event callback failures use visible consumer error/incomplete state; allocation
return values are not error codes. No callback may silently lose records.

The [full declaration sketch and ownership/error rules](profiling-sampling-design.md#proposed-c-declarations)
include the descriptor layout, registration functions and differences from
Daan's current header. The sketch is not binary-compatible with that header.

---

## 6. USDT and in-process consumers accumulate equivalent logical information

**USDT moves state; it does not make profiling stateless.**

| Logical information | USDT/eBPF route | In-process route |
| --- | --- | --- |
| Sampling decision | Theap sampler | Theap sampler |
| Recognition of sampled frees | Allocator metadata | Allocator metadata |
| Allocation context | eBPF unwinder at the probe | Native/runtime provider at the callback |
| Live identity and original contribution | External correlation maps/records | Retained allocation cookie and consumer records |
| Stack aggregates | External profiler | Consumer-owned local shards and merged state |
| Reporting | OTel collector pipeline | File writer, optionally another exporter |

The structures need not be identical. In particular, cumulative-only profiling
does not require the live-set machinery.

Both routes need explicit treatment of event loss, memory bounds, lifetime
ordering, and incomplete profiles.

---

## 7. How the OTel eBPF profiler sees the hooks

```text
mimalloc sampled callback
    -> thin adapter computes the agreed byte contribution
    -> otel_memory:alloc(user_pointer, size, weighted_bytes)
    -> eBPF captures stack and emits/correlates event
    -> userspace profiler aggregates and exports OTLP Profiles

mimalloc sampled free
    -> otel_memory:free(user_pointer)
    -> external live set removes the original allocation
```

The initial [OTel design][otel-memory] derives objects as
`weighted_bytes / size` for positive sizes. Size basis and weight must agree.
Zero-request objects and in-place resize need explicit treatment.

USDT notes and semaphore/attachment handling are adapter concerns. Never defer
the allocation probe to a flush: the unwinder would capture the flush stack.
The free probe has no weight or stack argument; the profiler retains the
allocation's information.

External rate limiting and ring-buffer loss are separate from allocator
sampling. Their accounting belongs to that consumer.

---

## 8. In-process accumulation: one cold theap pointer, consumer-owned shards

Conceptual structure; no new hot-path stack-table lookup:

```text
mi_theap_t
    remaining, mean, existing RNG
    profiler_local ----------------> profiler_shard
                                         session reference
                                         stable stack-entry table
                                         synchronization / pending frees
                                         publication and retirement state

sampled-block cookie ------------> sample_record
                                      shard + stable stack entry
                                      original assigned contributions

profiler_session
    merged counters keyed by stack
    active and retired shards
    module identities and snapshot/output state
```

Allocate the shard lazily or at an explicit initialization point. Its storage
belongs to the optional consumer and may outlive the theap.
An allocation callback needs access to that local state without another
common-path TLS lookup. Free uses the original retained record, not the
freeing thread's local state.

Local stack interning must be allocation-safe. Alternatively, capture records
locally and intern them in a worker; that changes the consumer, not the sampling
contract. Never retain a pointer into a hash-table slot that can move on resize.

---

## 9. What merges, and what must survive the merge

Each stack entry accumulates:

```text
pending_alloc      = { objects, requested_bytes, usable_bytes }
pending_live_delta = { objects, requested_bytes, usable_bytes }

allocation: pending_alloc += contribution; pending_live_delta += contribution
free:                                      pending_live_delta -= contribution

publish: global[stack] += transferred local deltas
         clear only the transferred deltas
```

**Do not clear entries or free records still referenced by live allocations.**
Map local stack identities to the session dictionary before combining them.
Preserve module identity so reused addresses do not merge unrelated code.

Remote frees require synchronization: a sampled-path shard lock is a simple
baseline; owner-directed queues are an optimization candidate. On thread exit,
transfer shard ownership instead of waiting for future activity on a dead theap.

Existing collection/merge points are publication opportunities, not the
implementation of this merge. They provide neither an all-thread barrier nor
a guaranteed idle-thread flush. A snapshot must coordinate pending allocation
and free deltas; never clamp a temporarily negative delta and call it complete.

---

## 10. Unwinding is a provider choice, not an allocator dependency

| Route | Candidate | Constraint |
| --- | --- | --- |
| In-process native | Platform unwinder | Allocation/reentrancy safety, initialization, module access |
| In-process frame pointers | Small frame-chain walker | Compiler/platform coverage; safe bounded reads |
| Runtime integration | Supplied logical/native frames | Document attribution and mixed-stack behavior |
| External | Existing OTel eBPF unwinder | Captures at probe time; collector capability/coverage |

A file-consumer interface can request frames into caller-owned bounded
storage and return frame count plus complete/truncated/failed status.
Specific providers and supported platforms remain open.

Unwinding finds frames. Symbolization gives those frames names and source
locations. They are different operations and need not occur together.

Do not assume a generally available `backtrace` API is allocation-safe.
Partial stacks and unwind failures must be visible, not silently excluded.

---

## 11. File formats and OTel support are separate decisions

| Capability | Proposed position |
| --- | --- |
| Standard pprof file | Initial target: gzip-compressed pprof protobuf |
| OTel memory USDT contract | Shared-hook adapter target, independent of file format |
| Native OTLP Profiles export/storage | Possible later consumer/exporter capability |
| Arbitrary gzipped OTLP file | Do not describe a private convention as an OTel standard |

pprof can place multiple values on one stack sample, including
`alloc_objects`, `alloc_space`, `inuse_objects`, and `inuse_space`.
Define which byte measure the standard space fields carry; requested and
usable bytes must not be silently interchanged.

Current OTLP Profiles uses one sample type per profile and a shared dictionary,
plus resources/scopes and richer metadata. It is not just uncompressed pprof.
Converting ordinary stacks/counters is feasible; arbitrary metadata does not
necessarily round-trip.

The Collector file exporter implements OTLP JSON or framed protobuf and
optional zstd compression; profile support is Development. The current
[OTLP file specification][otel-file-spec] does not yet cover profiles.
See the [Collector implementation][otel-files] for its file conventions.

---

## 12. A profile can carry its own symbols, but need not

**Both pprof and OTLP Profiles can contain symbolized stacks.**

Initial proposal: raw PCs plus mappings, file offsets, module paths and build
identities where available, with matching binaries/debug artifacts retained
separately. Existing tooling can then symbolize offline; verify platform
support rather than assuming every debug format works everywhere.

An optional symbolized artifact can embed function names, source locations,
and inline information. Displaying those fields then needs no binary, but
source text and disassembly still need their corresponding artifacts.

Capture module identity over time, not merely at final dump: libraries can
unload, addresses can be reused, and JIT code can change. A filename alone is
not sufficient identity.

Symbol lookup, compression, and file writes stay outside allocator callbacks.

---

## 13. Tiny allocator boundary; realistic consumer obligations

| Keep in the common allocator paths | Keep off them |
| --- | --- |
| Existing theap lookup and compact selection state | RNG draws and weighting conversion |
| Minimal local update and predictable branch | Stack capture, interning, queues/locks |
| Existing sampled-free recognition machinery | Symbolization, protobuf, compression, I/O |

Measure compiled-out, inactive, active/non-sampled, and sampled costs
separately. Sampling can also change size classes and page flags, affecting
co-resident unsampled objects.

Bound consumer storage and expose loss/overflow/output errors. Distinguish an
intentional statistical non-selection from a missing allocation or free.
Agree on start/pause/snapshot boundaries; do not claim exact process-wide
coverage from lazy per-thread activation.

Free-list-scheduled class sampling is a possible later fast-path optimization.
It needs a selection proof across refills, page changes and reclamation; it
is not the same as sampling a later stack after flushing coarse statistics.

---

## 14. Reuse and a staged first contribution

[one-collect][one-collect] offers useful Rust stack/string interning, pprof
export, and Linux uprobe/perf collection infrastructure. Evaluate reuse behind
the consumer boundary, not inside ordinary allocator paths. The inspected
snapshot is not a ready-made OTel memory-USDT/live-heap consumer.

| Stage | Deliverable | Keep open |
| --- | --- | --- |
| Contract and reference sampler | Defined sampling inputs, final-pointer/lifetime behavior, statistical and fast-path harness | Later selection optimizations |
| Consumer integration | Optional local-state lifecycle; simple correct accumulation and snapshot | Lock versus queued publication optimization |
| First file profile | Native stacks, cumulative/live counters, unsymbolized pprof | Unwinder/provider and encoder implementation choices |
| USDT adapter | Same selection/lifetime semantics for OTel's probe contract | External collector implementation |
| Additional capabilities | Embedded symbols or native OTLP exporter if justified | Format/tooling scope |

Proposed packaging is an optional Rust file consumer with a small C ABI.
The allocator remains C with no Rust dependency in ordinary builds.
No commitment to a particular handwritten or generated encoder is necessary
to agree on the allocator contract.

---

## 15. Decisions to take to Daan

1. **Allocator mechanism:** can we preserve the fast-path layout while making
   randomized selection, original sizes and sampled lifetime ownership precise?
2. **Local consumer state:** is one cold opaque theap pointer plus optional
   initialize/publish/retire operations an acceptable boundary?
3. **Statistics versus profiles:** which owner-thread collection/teardown points
   can safely publish consumer state without new callback-under-lock hazards?
4. **Statistics contract:** start with TCMalloc-style weighting and charge
   `requested + 1`, while retaining inclusion-weighting capability?
5. **Lifetime semantics:** agree on activation, pause/drain, resize, heap
   migration/destruction and original-registration retention.
6. **Scope and evidence:** agree on initial platforms, byte measure, unwinder
   provider, and measured regression budgets before committing the public ABI.

**The ask is not for Daan to implement every layer. It is agreement on a small
allocator contract that makes those layers possible.**

---

## Source and implementation notes

Branch status above is scoped to the inspected revision, not a claim about
future `dev3-profile` changes. API operations and structures in this deck are
proposals. Mathematical reference models are not claims of bit-for-bit
equivalence with TCMalloc or jemalloc's finite-precision implementations.

- [Detailed mimalloc proposal](profiling-sampling-design.md)
- [TCMalloc sampling](https://github.com/google/tcmalloc/blob/master/docs/sampling.md)
- [jemalloc profiling internals](https://github.com/jemalloc/jemalloc/blob/dev/doc_internal/PROFILING_INTERNALS.md)
- [OTel memory-profiling design][otel-memory]
- [pprof schema and on-disk convention][pprof]
- [OTLP Profiles schema][otlp-schema]
- [OTLP file specification][otel-file-spec] and [Collector file exporter][otel-files]

[mi-api]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/include/mimalloc-profile.h
[mi-layout]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/include/mimalloc/types.h#L596-L682
[mi-sample]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/src/alloc.c#L963-L1006
[mi-stats]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/src/page.c#L146-L240
[otel-memory]: https://github.com/open-telemetry/opentelemetry-ebpf-profiler/blob/main/design-docs/00003-memory-profiling/README.md
[pprof]: https://github.com/google/pprof/blob/6331bc6350fe55a6fec2957299e0581dd7510e36/proto/profile.proto
[otlp-schema]: https://github.com/open-telemetry/opentelemetry-proto/blob/09f8394ecb171e889029fcd0036a3ab856c9b801/opentelemetry/proto/profiles/v1development/profiles.proto
[otel-file-spec]: https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/protocol/file-exporter.md
[otel-files]: https://github.com/open-telemetry/opentelemetry-collector-contrib/tree/main/exporter/fileexporter
[one-collect]: https://github.com/microsoft/one-collect/tree/36822a8ddf9daba345b9604394d8f17805fce383
