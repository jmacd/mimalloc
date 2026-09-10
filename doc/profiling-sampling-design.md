# Proposal draft: a small shared profiling mechanism for mimalloc

**Status:** discussion draft, not an accepted API or implementation specification.

This draft proposes an allocator integration and statistical contract shared
by an in-process file profiler and an eBPF adapter. The objective is a small
mechanism that fits mimalloc's fast paths, not a profiling framework inside the
allocator. Proposed interfaces and optimization directions below are not
accepted API decisions.

The implementation observations refer to `dev3-profile` at commit
`eaaed425687ba8fabd215b350f7e24f6d7cf6317`, not to the current checkout or to a
released profiling API.

For the architectural discussion and decisions to take to Daan, see the
[presentation draft](profiling-presentation.md). It separates the shared hooks,
sampling policy, consumer state placement, aggregation, unwinding, formats,
and symbolization.

## Proposed direction

Use a per-thread randomized byte countdown and TCMalloc-style
mean-plus-overshoot weighting as the preferred initial implementation. This is
a proposal preference, not an accepted upstream decision. Keep the hook
contract sufficient for inclusion-probability weighting over the same
selections as well; choosing the initial estimator need not lock the API to it.
Give each sampled allocation explicitly defined sampling metadata that both
consumers can use without reinterpreting allocator accounting counters.

Start from the branch's fine-grained allocation accounting. Do not assume that
randomizing the existing coarse accounting produces the same sampling model.
Treat the fine-grained path as the correctness and performance baseline, not
as a requirement to ship a particular instruction sequence. A forward-scheduled
size-class sampler, described below, is a separate optimization candidate.

**Make the selection and capture its allocation context now; batch publication
and aggregation later.** A statistics flush must neither select a past
allocation retroactively nor reset the sampling process.

Keep the following quantities distinct:

- The configured **mean sampling interval**.
- The **random distance** drawn for the countdown.
- The **remaining distance** at an allocation boundary.
- The allocation's **charge against the sampling counter**.
- The **requested and usable sizes** being measured.
- The **statistical weight** assigned to the sampled allocation.

## Fit with mimalloc's architecture

The branch has two different kinds of heap. A `mi_heap_t` is a shared logical
heap; a `mi_theap_t` is its thread-local allocation state. A thread can have
multiple theaps, and a heap can have multiple threads allocating through it.
The natural ownership unit for sampling is therefore a theap, not a new
process-global counter or another independently looked-up TLS object.

The relevant existing mechanisms are:

| Mechanism | Implementation observation | Consequence for the proposal |
| --- | --- | --- |
| Thread-local fast path | `mi_theap_t.sample_countdown` precedes `pages_free_direct`; the page lookup deliberately depends on that layout. | Reuse the existing hot field and TLS lookup. Keep configuration and consumer state out of that layout. |
| Cheap allocation statistics | `mi_page_malloc_zero` increments packed used/allocation counts together with `xused.used_alloc += 0x10001`. | Preserve this optimization; profiling must not require full statistics on each allocation. |
| Deferred page accounting | `_mi_page_update_stats` derives allocation/free totals from those counts and page block size. | These totals contain volume, not the historical allocation stacks or exact requested-size sequence. |
| Deferred heap accounting | Collection updates page statistics and optionally merges theap statistics into the heap. Thread detachment also merges them. | A useful pattern for amortizing work, but not a profile snapshot or a wall-clock flush guarantee. |
| Random state | Each theap already has a random context and `_mi_theap_random_next`. | No new RNG or OS randomness request is needed on the ordinary allocation path. |
| Sample lifetime metadata | Profiled blocks contain a tag and consumer data; both local and remote generic free paths dispatch through the profile check. | Keep free recognition allocator-native; do not add a global lookup on every free. |

Sources: [theap/heap layout][mi-layout], [allocation fast path][mi-fast],
[page accounting][mi-page-stats], [collection][mi-collect],
[thread detachment][mi-detach], and [free dispatch][mi-free].
The layout-dependent lookup is in [the small-page helper][mi-page-lookup].

### Three independent activities, not one periodic flush

```text
allocation thread, at the allocation:
  select -> finalize pointer/size -> notify -> return to application
                                      |
                                      +-> USDT: eBPF captures the current stack
                                      |
                                      +-> file consumer: capture PCs and retain record
                                                                  |
consumer-controlled publication/aggregation ----------------------+
  publish batches -> merge counters/records -> snapshot -> pprof file

allocator statistics:
  page counts -> theap stats -> heap stats       (unchanged, independent)
```

The USDT cannot be deferred until a batch flush: the existing eBPF unwinder
would observe the flushing call stack rather than the allocation call stack.
The file consumer can defer processing only after preserving the stack or the
unwind context at allocation time. For the initial native file consumer,
capturing PCs synchronously is the simpler boundary.

Neither sampling correctness nor notification should depend on `MI_STAT`.
In particular, the exact sampler must work when normal statistics are disabled.
Existing collection can be an opportunistic publication point, but an idle
thread might never reach it. A profile dump needs its own documented snapshot
boundary; `mi_collect` is not a process-wide flush barrier.

## Proposed minimal allocator mechanism

### State and ownership

Use the existing fields where practical; the names below describe roles, not
additional fields to append mechanically:

| Location | State | Access frequency |
| --- | --- | --- |
| Theap, hot | Remaining distance to the next sampling slow path | Ordinary allocation |
| Theap, cold | Mean interval and, if needed, cached configuration generation/owner | Initialization, sampled path, administrative path |
| Theap, optional cold | Opaque consumer-local state pointer | Sampled paths and explicit local-state lifecycle |
| Theap, existing | Random context | Interval generation, not every allocation |
| Sampled block only | Final requested/usable sizes, stable profiler ownership and consumer cookie | Sampled allocation/free |
| Consumer | Stack storage, weights, aggregates, buffers, file state | Outside ordinary allocator paths |

In a profile-only configuration, mean and remaining distance are enough
statistical state. There is no need to accumulate all bytes since a statistics
flush. Existing guard-page sampling shares the countdown machinery, however;
coexistence must preserve separate logical deadlines. A guard event must not
redraw the profiler's interval or erase its elapsed charge. Combining deadlines
behind one hot counter is an implementation optimization requiring its own
state-transition checks, not a reason to conflate the two samplers.
Sharing a deadline also requires compatible charge units; introducing `r + 1`
for profiling must not silently change guarded-allocation semantics.

Start with the existing theap RNG behind one cold interval-generation helper.
The generator excludes zero from its output and is also used for allocator
security; its output domain must be accounted for when mapping to a geometric
distribution. Reusing it must not expose its raw state. An additional PRNG,
large lookup table, or new math-library dependency should need evidence that
the benefit justifies the footprint. Distribution generation, tail handling,
and finite-precision error remain explicit implementation decisions.
See [theap initialization][mi-theap-init] and [the RNG interface][mi-rng].

### Common path and sampled path

The reference operation for an active, initialized profile-only theap is:

```text
x = checked_sampling_charge(original_request)
d = remaining

if x < d:
    perform ordinary allocation
    on success: remaining = d - x
else:
    enter sampled slow path:
        reserve/finalize the allocation without recursively sampling it
        on failure: report no allocation and preserve the unconsumed distance
        on success:
            W = mean + x - d
            install sampled-block identity and retained owner
            notify(final_pointer, original_sizes, mean, W)
            apply any returned mean change to the next interval
            remaining = draw_interval(mean)
```

This is an ordering contract, not literal C to insert after every malloc.
The normal free-list-pop path cannot fail once a valid block is available, so
its countdown update can be placed for good code generation. Slow allocation
failure must not consume a successful-allocation sample or trigger a callback.
Internal retries, metadata allocations, and alignment over-allocation must
not charge the same application request twice.

Callbacks are not permitted to recursively enter the profiled allocator.
Provide an allocation-safe callback contract and suppression for intentional
profiler-internal work; unexpected recursion must be diagnosed. The
allocation/free of existing application samples must not be silently discarded
under a generic recursion guard.

Target incremental common-path work is a local countdown load/update and a
predictable branch, using the already-resolved theap. No global atomic, lock,
clock read, random draw, indirect callback, floating-point operation, or stack
walk belongs on an unsampled allocation. These are design constraints, not an
unmeasured claim that the additional instructions are free.

### Small API delta to discuss

The branch already passes a rate and a 64-bit sampling-related quantity to
`on_alloc`. A compact TCMalloc-style candidate is:

- Add an explicit initial mean to profiler configuration; do not force a
  synthetic first sample just to ask the callback for a rate.
- Define the rate argument as the mean that governed this selection.
- Replace the ambiguous `bytes_since_last_sample` contract with explicitly
  named `weighted_charge = W`. With a fixed documented `x = r + 1`, the consumer
  can recover the denominator from the original requested size.
- Keep a nonzero callback return as a proposed mean for the next interval,
  not a random distance. Always redraw using that mean at the boundary.
- Retain the existing per-sample consumer storage. Do not require mimalloc to
  retain stacks, floating-point weights, or output-format structures.

This is a candidate API revision, not a silent reinterpretation of existing
arguments. Under the reference geometric sampling law, exposing `R`, `x`
(possibly implicit in the requested size), and `W` permits either estimator:

```text
TCMalloc-style object weight:  W / x
Inclusion object weight:       1 / (1 - (1 - 1/R)^x)

requested-byte contribution = requested_size * object_weight
usable-byte contribution    = usable_size    * object_weight
```

The sampling law is part of the contract: a mean alone cannot establish an
inclusion probability for an arbitrary sampler. Conversely, an inclusion
probability alone does not recover the realized overshoot needed for
TCMalloc-style weighting. Keep the metadata sufficient for both, without
requiring two samplers or an estimator-mode branch in the allocation fast path.
Use shared conversion logic and retain each consumer's chosen contribution for
that allocation's lifetime. Selecting a different estimator must not silently
reweight outstanding live records.

For the first implementation, use a fixed mean per active theap epoch.
Global configuration changes can be adopted at existing owner-thread safe
points, with a fresh draw and a clearly defined activation boundary. Do not
write another thread's ordinary countdown concurrently. Lazy adoption avoids
a per-allocation configuration load but cannot promise immediate process-wide
start/stop; a synchronous boundary requires an explicit coordination mechanism.

### Proposed C declarations

This is a discussion-only replacement sketch for `mimalloc-profile.h`, not
an additional header to include alongside the branch's existing definitions.
It is not binary-compatible with that header. Sizes and `user_data` follow
the existing layout; the selection descriptor and local-state operations
make previously implicit contracts explicit.

```c
#include <mimalloc.h>
#include <stddef.h>
#include <stdint.h>

typedef struct mi_profiler_data_s {
  size_t usable_size;
  size_t requested_size;
  void*  user_data[6];
} mi_profiler_data_t;

typedef struct mi_profiler_sample_s {
  size_t   mean_interval;       /* R governing this selection */
  size_t   sampling_charge;     /* x, excluding profiler storage */
  uint64_t weighted_charge;     /* W = R + x - d */
} mi_profiler_sample_t;

typedef int (mi_cdecl mi_profiler_local_init_fun)(
    const mi_heap_t* heap, void* profiler_arg, void** local_arg);

typedef int (mi_cdecl mi_profiler_local_publish_fun)(
    void* local_arg, void* profiler_arg);

typedef void (mi_cdecl mi_profiler_local_retire_fun)(
    void* local_arg, void* profiler_arg);

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

typedef struct mi_profiler_s {
  void* reserved1;
  void* reserved2;
  void* reserved3;
  void* profiler_arg;
  size_t profiler_data_size;
  size_t initial_mean_interval;
  mi_profiler_on_alloc_fun* on_alloc;
  mi_profiler_on_free_fun* on_free;
  mi_profiler_on_realloc_inplace_fun* on_realloc_inplace;
  mi_profiler_local_init_fun* on_local_init;
  mi_profiler_local_publish_fun* on_local_publish;
  mi_profiler_local_retire_fun* on_local_retire;
} mi_profiler_t;

#ifdef __cplusplus
extern "C" {
#endif

mi_decl_export bool mi_heap_profile(
    mi_heap_t* heap, const mi_profiler_t* profiler);
mi_decl_export bool mi_subproc_profile(
    mi_subproc_id_t subproc_id, const mi_profiler_t* profiler);
mi_decl_export bool mi_profile(const mi_profiler_t* profiler);
mi_decl_export bool mi_profiler_start(const mi_profiler_t* profiler);
mi_decl_export bool mi_profiler_stop(const mi_profiler_t* profiler);

#ifdef __cplusplus
}
#endif
```

**Selection descriptor.** The first revision specifies the geometric reference
law in this document; these three fields are not sufficient for arbitrary
undocumented distributions. `sample` is borrowed for the allocation callback
only. It can be stack-constructed on the slow path; it need not enlarge every
sampled-block header. The consumer retains the contribution or inputs it needs
in its own record. Explicit `sampling_charge` avoids requiring the consumer
to infer it from an internal over-allocation size.

**Allocation callback.** As in Daan's branch, zero return means retain the
current mean; nonzero requests the next interval's mean for this theap.
`initial_mean_interval` is positive. The first implementation can keep it
constant by returning zero. Requested/usable sizes are allocator-owned fields;
the consumer owns only its configured `user_data` region. The initializer
must ensure `profiler_data_size` covers every user slot it accesses; allocating
less than the default structure does not make all six slots available.

**Local-state lifecycle.** The proposed operations are optional as a group.
With no local-state hooks, allocation callbacks receive `local_arg == NULL`,
which is sufficient for a USDT-only adapter. Otherwise:

| Operation | Contract |
| --- | --- |
| `on_local_init` | Called before this local context's first allocation callback. Returns zero on success, nonzero error code on failure; initializes `*local_arg` on success. Failed initialization cleans up its own partial resources. |
| `on_local_publish` | Requests publication of pending deltas. Returns zero on success, nonzero error code on failure. A failure must not lose or replay partially transferred deltas. This is not a process-wide snapshot barrier. |
| `on_local_retire` | Called once for each successfully initialized context when mimalloc relinquishes it, even if the context pointer is null. Transfers ownership to the consumer; does not imply that outstanding allocations or remote frees are finished. |

Initialization and ordinary publication use owner-thread safe points.
Retirement may execute during heap destruction on another thread, after local
allocation/publication callbacks are quiescent. No lifecycle callback may be
invoked while holding an allocator lock that it could reenter or invert.
Remote free callbacks can remain concurrent with publication and retirement;
the consumer must synchronize its retained shard accordingly.

Nonzero lifecycle results must reach mimalloc's error-reporting path and the
consumer's visible incomplete/error state. They are not application allocation
failures. Failed initialization disables recording for that local context
until an explicit retry/reinitialization policy applies; it must not silently
resume as though the missing interval were observed.

**Free and resize.** Neither receives the current thread's local context or
heap as its ownership token. `data->user_data` identifies the original
retained record and shard; `profiler_arg` comes from the originating pinned
registration. The allocator stores that registration identity separately from
consumer-owned slots. `data` is valid only for the callback, especially on
free. Queued work must not retain a pointer into the freed allocation header.

For in-place resize, `data` contains the new sizes and the explicit arguments
contain the old sizes. The stored selection/contribution belongs to the
original allocation. No mean change is returned for resize, unlike the branch's
declaration: a resize notification is not a newly selected allocation.
Its cumulative accounting and mapping to OTel's alloc/free-only contract
remain an open semantic decision.

**Errors in event callbacks.** The allocation return value remains a mean,
not an error channel; free and retire cannot be cancelled. The consumer must
record and surface capture/storage failures through its session status and
error reporting. It must initialize every allocation cookie to either a valid
record or an explicit unrecorded sentinel before returning; a later free of
that sentinel must be recognized without pretending the allocation was
successfully recorded. The file consumer's dump/status API must report such
incompleteness. A future explicit callback-status ABI is an alternative if this
side channel is undesirable.

**Registration and control.** These function names retain the branch's scope,
not a claim that its current stop/replacement implementation is sufficient.
Reserved fields start at zero; descriptors and shared state have process
lifetime in the first proposal. Registration rejects unsafe replacement;
stop pauses new selection after owner-thread adoption but keeps original
sample lifetime callbacks. Start/stop retain the branch's previous-running-state
return convention, not a successful-allocation or synchronous-barrier result.

### Cheap when disabled is a separate requirement

Distinguish support compiled out, support present but inactive, and active
sampling. A large disabled countdown still incurs its load/update in a
fine-grained build; it is not zero overhead, and it eventually expires.
Preserve a no-profile build and measure the inactive case separately.

Similarly, the existing administrative cadence is based on generic allocation
calls (a check after 1,000 such calls), not elapsed time or every 1,000 user
allocations. Reusing it for configuration discovery is plausible; promising a
bounded time to activation or publication on that basis is not.

## A lower-hot-path-cost direction worth investigating

The reference byte countdown is not the only possible correct sampler.
Mimalloc's free-list fast path already branches when `page->free` is empty,
and local frees accumulate separately. A more allocator-specific design could
schedule a future sample by limiting a free-list run and reaching the existing
slow path at a selected allocation ordinal. The relevant starting points are
[free-list collection][mi-refill] and [free-list extension][mi-extend].

For a size class with positive scheduling charge `b`, choose a known per-object
probability, for example:

```text
p_b = 1 - exp(-b/R)
next sampled allocation ordinal ~ Geometric(p_b)

sample contribution:
    objects = 1/p_b
    requested bytes = actual_request/p_b
```

This deliberately samples according to class charge, not exact requested
bytes. It can still estimate requested bytes correctly if each allocation has
the stated inclusion probability. A cached class weight may also avoid
per-sample transcendental evaluation. This is a different design from applying
Poisson weights to the next stack after a statistics flush.

The current code has multiple refill/merge paths, page switching, abandoned
page reclamation, and encoded free lists. Simply truncating a list is not a
complete or safe implementation. An experiment would need to establish:

- The selected ordinal reaches the callback for that application allocation,
  including when a natural list exhaustion occurs first.
- Local and remote free-list merges cannot bypass or postpone the scheduled
  sample in a workload-dependent way.
- Switching pages, retiring/reclaiming pages, and changing heaps preserve the
  declared conditional selection probability.
- The hidden free-list tail remains correctly accounted for and restored;
  sampling does not masquerade as memory exhaustion or force needless pages.
- Scheduling uses the uninstrumented allocation's class, not a larger class
  introduced by the sampled header; aligned/huge requests have defined paths.

Prefer the simpler byte-countdown implementation unless this experiment shows
a worthwhile inactive/active fast-path improvement at acceptable code and
metadata cost. Keep the hook contract expressed in statistical contributions
so it does not prevent such an optimization.

## What the profile must estimate

Hold an application's successful allocation history fixed, including allocation
stacks, sizes, and lifetimes, and consider repeated sampling with different
random seeds. For allocation `i`, define:

```text
s_i = bytes being measured
I_i = 1 if the allocation is sampled, otherwise 0
w_i = its reported byte contribution when sampled

Required: E[I_i * w_i] = s_i
```

This per-allocation property gives the correct expected total for a fixed group
of allocations: a stack, a thread, or the objects still alive at a snapshot.
It does not promise an exact result from an individual profile. Variance still
determines how much sampling noise users see.

Correct whole-process totals alone are insufficient. Consider a deterministic
sampler that chooses every hundredth equal-size allocation from this repeating
sequence:

```text
99 allocations from stack A
 1 allocation  from stack B
```

If its sampling position always lands on B, assigning all hundred allocations'
bytes to B gives the correct total and a completely wrong attribution.

## Two valid estimators

### Inclusion-probability weighting

If an allocation has known, nonzero inclusion probability `p`, an estimated
object contribution of `1/p` and byte contribution of `s/p` satisfy:

```text
E[I * (1/p)] = 1
E[I * (s/p)] = s
```

For a continuous Poisson process over allocation volume, with mean interval `R`
and allocation charge `x`:

```text
p = 1 - exp(-x/R)
```

That formula belongs to that sampling model. In particular, it must not be
applied blindly to a deterministic countdown or to an arbitrary batched
accounting scheme.

### TCMalloc-style mean-plus-overshoot weighting

An alternative avoids computing an inclusion-probability division to obtain the
sampling charge represented by an event. Let:

```text
R = configured mean interval
x = allocation's charge against the sampling counter
d = remaining distance to the first sampling point at allocation entry
```

If `d <= x`, sample the allocation and assign:

```text
W = R + x - d
```

The first sampling point represents `R` counter units. The remaining `x - d`
units contribute their expected additional sampling weight. Emit at most one
event for the allocation, even if it could contain multiple sampling points.

For the corresponding randomized model:

```text
E[I * W] = x
E[W | I = 1] = x/p
```

Thus this estimator and inclusion-probability weighting have the same
conditional mean, but different per-sample values and variance.

**The first term in `W` is the configured mean, not the particular random
distance drawn at the previous sample.** Observed bytes since the previous
sample are not automatically interchangeable with this weight.

### Estimator derivation and worked example

Selection and estimation are separate. Fix an allocation's charge `x > 0`
and any measured property `y` independent of the profiler's random choices.
Let `I` indicate selection. An object expansion weight `h` must satisfy
`E[I*h] = 1`; then `I*y*h` estimates `y`. This is why object count, requested
bytes, and usable bytes can share one sampling process. Sampling-induced
changes to allocation placement require a separately agreed usable-size
definition; a counterfactual uninstrumented size is not automatically observed.

For the discrete reference model below, imagine independent Bernoulli points
in each of the allocation's `x` charge units. Let `N` be their count:

```text
N ~ Binomial(x, 1/R)
E[R*N] = x
```

If no point occurs, report zero. If the first occurs at distance `d`, the
remaining `x-d` independent units contain `(x-d)/R` points in expectation.
Conditioning avoids generating those additional points:

```text
E[R*N | first point at d] = R * (1 + (x-d)/R) = R + x-d = W
E[I*W] = x
```

Further conditioning only on whether a point occurred gives:

```text
E[I*W | I] = I*x/p
E[W | I=1] = x/p
```

Thus the three charge estimators `R*N`, `I*W`, and `I*x/p` have the same
expectation. Each successive conditional average removes variance. We prefer
the middle form initially for its implementation shape, not because the last
is statistically inferior. Inclusion weighting is Horvitz-Thompson weighting;
the conditional-averaging comparison here is often called Rao-Blackwellization.
Unbiased sums require correct marginal contributions, not independence among
allocations. Variance of a sum additionally depends on covariance.

For example, set `R=4`, `x=2`, so `p=7/16`:

| Outcome | Probability | `I*W/x` (objects) | `I/p` (objects) |
| --- | --- | --- | --- |
| `d=1` | `1/4` | `5/2` | `16/7` |
| `d=2` | `3/16` | `2` | `16/7` |
| `d>2` | `9/16` | `0` | `0` |

The expected object contribution is one for both. If `r=1`, `a=8`,
`x=r+1=2`, multiply these values by one for requested bytes or eight for usable
bytes. The respective expectations are one and eight.

The charge variances in this example are `21/4` for overshoot and `36/7` for
inclusion weighting. Object variance is charge variance divided by `x^2`.
The example uses a small mean to make the arithmetic visible, not as a
recommended production sampling rate.

### Numerical and operational interpretation

For a continuous exponential model the formulas instead use
`p = 1-exp(-x/R)` and `Var(I*W) = R^2*p`. Use the law actually implemented;
integer offsets and bounded random precision are not interchangeable with an
ideal continuous random variable.

For numerical evaluation, `-expm1(-x/R)` avoids cancellation in the continuous
probability. For the discrete probability with `R>1`, use
`-expm1(x*log1p(-1/R))`; handle `R=1` separately as `p=1` for positive charge.
These are reference evaluation formulas, not a requirement for math-library
calls on every sampled allocation. Cached values and approximations need
documented error bounds.

For a fixed group of allocations, sum the assigned contributions to estimate
its totals. A live profile uses the same contributions restricted to surviving
objects; a free removes the exact contribution originally added. Ordinary
heap-stat flush boundaries have no role in that estimator.

Neither estimator corrects for missing callbacks, truncated capture coverage,
lost frees, or records dropped because storage is full. These are additional
observation mechanisms, not the sampling probability `p`. Report them explicitly.

## Reference sampling model

For unambiguous integer-byte semantics, use the following as a reference model,
not yet as a prescription for the random-number implementation:

- Each counter unit independently contains a sampling point with probability
  `1/R`, where `R >= 1`.
- Draw a geometric distance on `1, 2, ...`, with mean `R`.
- Initialize each active thread's countdown with such a draw.
- Examine every successful application allocation using its charge `x`.

```text
if x < remaining:
    remaining -= x
    do not emit a sample
else:
    W = R + x - remaining
    emit one sampled allocation with weight W
    remaining = draw_geometric(mean = R)
```

The fresh draw starts after the entire allocation. The unrepresented sampling
points inside the allocation have already been accounted for in `W`.

For this discrete model the exact inclusion probability is:

```text
p = 1 - (1 - 1/R)^x
```

The exponential expression is a related continuous model or approximation,
not an identical formula for every discrete sampler. Rounding, finite random
precision, interval caps, and efficient generation need their own treatment.

The hot path needs a thread-local countdown update and comparison, not a
random-number call for every allocation. Random generation and weighting
belong on initialization or sampled slow paths.

### Tradeoff behind the initial estimator preference

For the same discrete geometric selections and fixed charge `x`, let
`p = 1 - (1 - 1/R)^x`. The charge estimators have:

```text
Var(I * W)     = R * (R - 1) * p
Var(I * x / p) = x^2 * (1 - p) / p
```

Inclusion-probability weighting replaces the random conditional weight by its
conditional mean, removing that component of variance. Large allocations are
almost certainly selected and then have almost deterministic weights, whereas
mean-plus-overshoot retains noise of order `R`.

The appeal of mean-plus-overshoot here is a cheap integer numerator and a small
allocator interface, not superior statistical efficiency. Both approaches pay
for interval generation; avoiding a weight-side exponential does not eliminate
that cost. In fact, [jemalloc precomputes its unbiasing tables][je-unbias] by
size class: its normal sampled-object accounting does not evaluate an
exponential for every sample. A size-class-based mimalloc design could use the
same technique. Arbitrary exact requested-byte charges have a different
table-size/caching tradeoff.

Compare end-to-end cost at comparable accuracy as well as cost per event.
Lower variance can reduce required stack captures, while tables consume space
and complicate rate changes. The exact discrete model above is our reference,
not a claim of bit-for-bit equivalence with either allocator's finite-precision
interval generation, offsets, or rounded reported values.

## Sampling charge versus measured bytes

As an initial proposal, follow TCMalloc's requested-size-plus-one convention:

```text
r = application-requested size
a = application-usable size under an agreed accounting definition
x = r + 1

estimated_objects         = W / x
estimated_requested_bytes = r * W / x
estimated_usable_bytes    = a * W / x
```

The extra counter unit gives zero-byte allocations a nonzero chance of being
sampled. It is not an extra application-requested byte.

Profiler headers, stack records, and other profiler-owned storage must not be
charged as application bytes. The definition of `a` also needs to address
alignment and any size-class changes introduced by sampling.

These are mathematical quantities, not integer-division instructions.
Representation, accumulation, rounding, and overflow handling must not
silently truncate contributions or change their meaning.

## Relationship to the current branch

The inspected branch already exposes sampled-allocation metadata, allocation
and free callbacks, requested and usable sizes, and sampling-related arguments.
It also declares an in-place realloc callback.

Its sampling expression has the shape:

```text
requested =
    sample_rate
    + (req_size - sample_countdown)
    + sample_requested
```

That resembles mean-plus-overshoot weighting; the expression is not inherently
wrong. The unresolved issue is the meaning and evolution of its inputs:

- With a consumer returning a constant rate, the countdown resets
  deterministically rather than drawing fresh random distances.
- Returning random values through the callback would require distinguishing
  those distances from the configured mean used in the estimator.
- Default coarse accounting deducts accumulated page-allocation volume.
  Capturing a subsequent allocation's stack does not reconstruct the stacks
  responsible for that historical volume.
- Fine-grained accounting is available separately and is the proposed starting
  point for establishing the reference model.

No claim is made here that every coarse sampler must be invalid. A different
scheme needs its own inclusion or weighting argument; the equations above do
not establish one for it.

## Shared hook contract

The allocator integration and consumers should agree on these semantics before
fixing an ABI:

| Quantity | Required meaning |
| --- | --- |
| Allocation identity | The final application-visible allocation, stable for matching its lifetime events |
| Requested size | Original application request, excluding alignment over-allocation and profiler metadata |
| Usable size | A documented application-byte measure, distinct from requested size |
| Sampling charge | The units used to make the selection decision |
| Statistical contribution | A defined object weight and corresponding byte weights, or equivalent information with shared conversion logic |
| Mean interval | The mean governing the selection, not merely a random countdown realization |
| Lifetime state | Enough retained state to remove the allocation's assigned live contribution correctly |

This table describes meanings, not a proposed C struct. Whether the ABI carries
canonical weights, a numerator and denominator, or other estimator metadata
remains open. Consumers must not independently guess the meaning of
`bytes_since_last_sample`.

The OTel proposal accepts `alloc(user, size, weighted_bytes)` and derives object
counts as `weighted_bytes / size`. A TCMalloc-style producer can supply a byte
contribution consistent with the chosen size basis, but two details need
agreement:

- How the chosen estimator and its scalar encoding/rounding are documented
  against the contract's essential unbiased-byte requirement, rather than
  assuming the example inclusion-probability formula is the only estimator.
- How to represent zero-request-size allocations without a division by zero
  or silently losing their object counts.

Requested-byte versus usable-byte reporting must be an explicit choice shared
by the file and eBPF consumers.

## Lifetime and operational requirements

An allocation's assigned weight must survive changes in the configured rate.
Free must remove exactly its retained live contribution, including when another
thread performs the free. In-place resize needs a separately defined accounting
rule rather than treating it as an ordinary free.

Stopping an allocation-profiling session is different from detaching lifetime
tracking. Outstanding samples, callback completion, heap migration/destruction,
and profiler replacement need explicit ownership rules.

Forced startup samples, rate changes during an interval, and allocations made
before profiling starts cannot be treated as ordinary samples without defining
their selection semantics. Failed allocations must not be reported as
successful application allocations.

Unwind failures, exhausted record storage, lost events, counter overflow, and
file-output failures are not ordinary statistical non-selection. They must
produce explicit errors or visible incomplete-profile accounting. In particular,
losing a free can leave a false live allocation.

### Smallest safe first lifecycle

Prefer process-lifetime profiler descriptors initially, rather than requiring
dynamic code unloading and immediate descriptor reclamation. Pin the original
registration in sampled-block ownership; do not dispatch through whichever
profiler its page's heap happens to reference at free time. This accommodates
heap migration without adding work to unsampled frees.

Pause stops new selections once the owner thread observes it, but retains
callbacks for existing samples. Replacing a registration or detaching while
callbacks/samples remain must either be rejected explicitly or use a defined
draining protocol. A process-lifetime first version can avoid some reclamation
machinery without pretending that `stop` makes outstanding references safe to
destroy.

The free notification runs on the freeing thread before storage is reusable,
not when the allocating thread later collects its remote-free list. The
existing dispatch placement already supports this. A consumer must not depend
on the original theap or its TLS surviving until free.

Moving realloc naturally allocates a new object and releases the old one only
after success. In-place realloc is different: retaining an allocation's object
weight and updating its live byte contribution is possible for a file consumer,
but the initial OTel USDT contract has no resize operation. Synthesizing
`free + alloc` also changes cumulative allocation counts/bytes and stack
attribution. That conversion needs an explicit profiling semantic, not just
wiring the currently unused callback.

## In-process consumer context

The intended file consumer is an optional Rust library with a C ABI, providing
TCMalloc-style heap snapshots and allocation-profiling sessions. Native stack
capture occurs on sampled allocations; frees use the retained allocation record.
Callback storage and unwinding need an allocator-reentrancy contract.

Use standard pprof protobuf with gzip at the file-output boundary. A handwritten
encoder, generated encoder, or reuse of one-collect's exporter is a consumer
implementation choice, not a requirement on the allocator contract.
Do not symbolize, compress, or write files in allocation callbacks. An eBPF
adapter consumes the same statistical and lifetime semantics but uses its own
stack capture and OTLP reporting path.

These consumer choices do not determine the sampling ABI or require ordinary
mimalloc builds to depend on Rust.

### Per-theap sample accumulation

An optional cold `profiler_local` pointer in the theap can reference a
consumer-owned shard. This is distinct from the existing shared `profiler_arg`.
The current hook API has no explicit local-state lifecycle; optional
initialize/publish/retire operations are a proposed addition.
The allocation callback must receive or be able to access its initialized
local context separately from shared `profiler_arg`. A free callback instead
uses the original sampled allocation's retained record, not whichever local
context belongs to the freeing thread.

```text
theap.profiler_local -> shard
    session reference
    stable stack-entry table
    synchronization / pending cross-thread frees
    publication and retirement state

sampled allocation cookie -> retained record
    shard + stable stack entry
    assigned object/requested/usable contributions

session
    merged stack dictionary and counters
    active/retired shards, module identities, snapshot state
```

Each stack entry holds pending allocation totals and signed live deltas:

```text
on allocation: pending_alloc += contribution; pending_live_delta += contribution
on free:                                    pending_live_delta -= contribution
on publish:   global[stack] += transferred deltas; clear transferred deltas
```

Apply weighting before aggregation. Local stack IDs need translation into a
session-wide identity before merging; raw IDs from different shards are not
interchangeable. Module/build identity must also distinguish reused code
addresses. Counter arithmetic needs the same precision and overflow contract
as individual contributions.

Publication clears pending deltas, not stable entries or records referenced
by live allocations. The consumer must prevent table growth from invalidating
those references. Remote free can update a shard under a sampled-path lock or
enqueue a retained record for owner processing. Queued records cannot reside
in application storage that the allocator has already reused.

A shard can outlive its theap. Retirement transfers responsibility to the
consumer, which must continue processing pending frees and retain storage
until outstanding records/readers are finished. No algorithm may require the
dead allocating thread to perform a future flush.

Existing statistics merge points provide scheduling opportunities, not a
merge implementation for this variable-sized state. Profiling publication
must be independent of `MI_STAT` and `mi_option_collect_merges_stats`. Current
thread-detachment statistics merging can run under allocator locks: place
consumer calls only at explicitly safe lifecycle points, not mechanically
inside `_mi_stats_merge_into`.

Snapshot synchronization must include unpublished allocations and remote
frees. A global total plus independently read local totals is not sufficient
if a concurrent publication can be counted twice or omitted. A simple correct
sampled-path locking scheme is an acceptable first consumer implementation.

### What can be batched

The allocator does not need to own a queue, worker, periodic timer, or
per-stack aggregate. Let the file consumer own publication and retention:

- On a sample, capture the allocation stack and immutable sample contribution
  in allocation-safe storage. The sampled block retains a stable record cookie.
- On free, use that cookie to record the lifetime transition without unwinding.
  A stable record with synchronized live state is one option; an ordered
  lifetime-event stream is another. Neither may reference retired stack memory
  or assume the freeing thread already has profiler TLS.
- Either aggregate into allocation-safe local stack entries or publish captured
  records for worker-side interning/aggregation. Both fit the hook boundary;
  the former needs safe local tables, the latter needs more buffered records.
  Any consumer TLS lookup is on the sampled path only.
- Establish a snapshot cut across producers explicitly. Buffer-full and
  thread-exit publication are useful, but cannot flush a partially filled
  buffer on an idle thread by themselves.

Cross-thread free can occur before an allocation batch is consumed. Record
identity and publication ordering must prevent a late allocation record from
resurrecting a freed object. Pointer reuse makes a raw pointer insufficient as
an offline event identity without ordering or an allocation generation.
Record reclamation must also wait until concurrent snapshot readers are done.

Begin with a simple correct consumer synchronization scheme on sampled events;
do not make an unproven lock-free queue part of the allocator proposal.
Snapshot consistency, bounded storage, and failure reporting are consumer
requirements regardless of whether publication happens periodically.

## Allocator versus adapter responsibilities

Both consumers accumulate the same kinds of logical information: allocation
contexts, original contributions, live identities where requested, and stack
aggregates. The USDT route moves much of that state into eBPF maps and external
profiler userspace; it does not eliminate it. Sampled-free recognition remains
inside mimalloc in both cases. Their concrete data structures, synchronization,
loss modes, and reporting boundaries need not be identical.

| mimalloc | Optional shared profiler/USDT adapter | File consumer / external OTel profiler |
| --- | --- | --- |
| Selection at a real allocation boundary | Convert documented sampling metadata to contributions | Capture stacks using the consumer's mechanism |
| Final pointer and original sizes | Emit `otel_memory:alloc` and matching `free` | Retain/aggregate allocation lifetimes |
| Sampled-block recognition and owner lifetime | Handle USDT enablement without losing lifetime ownership | Report loss and enforce storage limits |
| Internal-allocation suppression | Keep size/weight conventions consistent across outputs | Symbolize, serialize, compress, write/export |

USDT notes, probe argument encoding, and semaphores belong to the optional
Linux adapter, not to the generic C hook contract. eBPF captures the stack at
the allocation probe; it does not need an in-process unwind first. Neither
pprof nor OTLP types belong in the allocator. Supporting two consumers at once
would require explicit dispatch because the current heap has one profiler
pointer; it is not a prerequisite for the first implementation.

## Patch outline and acceptance criteria

These are proposed implementation steps, not changes made by this document:

| Surface | Proposed change |
| --- | --- |
| `include/mimalloc-profile.h` | Define initial mean, sample charge/weight semantics, callback safety and retained ownership; name any ABI changes explicitly. |
| `include/mimalloc/types.h`, `internal.h` | Preserve hot field order; separate mean and remaining distance; keep inactive, guarded, and profiler state transitions coherent. |
| `src/theap.c`, `src/profile.c` | Initialize randomized state from the existing RNG; implement sampled metadata/notification and activation boundaries without forced statistical samples; define optional consumer-local initialize/publish/retire operations. |
| `src/alloc.c`, `src/page.c` | Charge once at the user-allocation boundary; decouple exact sampling from page-stat flushes and `MI_STAT`; preserve failure/retry behavior. |
| `src/alloc-aligned.c` | Keep original request and sampling decision through alignment; notify with final pointer, including large-alignment paths. |
| `src/free.c`, heap/arena lifecycle | Preserve immediate local/remote free notification and original profiler ownership across migration/destruction. |
| CMake/install, optional adapter | Expose supported build modes, install the public profiling header, and keep USDT/file dependencies opt-in. |

Evaluate the mechanism without an unwinder first, then with actual consumers.
The cost matrix must distinguish compiled-out, inactive, active/non-sampled,
sampled allocation, and sampled/unsampled local/remote free. Inspect generated
code and measure cycles, branches/misses, instruction footprint, theap/page
size, and sampled-block overhead on x64 and ARM64; a throughput average with
stack unwinding enabled cannot isolate a fast-path regression.

The branch's sampled allocation over-allocates an ordinary block and marks
its page as having interior pointers. Include size-class changes and the
effect on co-resident unsampled frees in that accounting; do not assume all
overhead is confined to sampled objects. Separate sampled pages would be
another placement policy, not a prerequisite for either estimator.

Use an agreed workload matrix: repeated tiny alloc/free, bursts with long-lived
objects, mixed sizes and stacks, many threads sharing heaps, multiple heaps per
thread, and cross-thread frees. Include `MI_STAT=0`, release/debug, guarded,
aligned/huge, and allocator-override configurations. Set numeric regression
budgets with Daan after measuring the baseline rather than inventing them here.

Correctness checks should exercise selected ordinals and known allocation
histories, not just whether a callback eventually fired. Compare stack-level
allocation and live totals across random seeds against the reference model;
include exact threshold equality, `R=1`, zero-size requests, repeated same-size
stacks, integer/tail boundaries, allocation failure, and configuration changes.
Verify final-pointer matching, failed/moving/in-place realloc, allocating-thread
exit, heap migration/destruction, pause and busy-detach behavior separately.

## Proposed decision order

Agree first that synchronous selection/context capture is independent of
deferred aggregation, and that original sampled-object ownership survives its
allocating theap. Next choose the tiny reference state and its charge convention.
Use that implementation to compare actual common-path cost with the
forward-scheduled size-class experiment. Start with the preferred TCMalloc-style
estimator while preserving enough hook information for inclusion weighting.
Finalize the public ABI against those requirements, rather than committing it
to the current coarse counters.

## Questions to resolve incrementally

1. Does the candidate metadata contract support both estimators without
   unnecessary fields or common-path work? TCMalloc-style weighting is the
   preferred starting implementation.
2. Is requested size plus one the desired sampling charge?
3. What precisely should usable-byte accounting measure for sampled and aligned
   allocations, and which byte measure should the standard profiles expose?
4. Where should random interval generation and weight calculation live, and how
   should the hook distinguish a mean interval from a sampled distance?
5. What discrete distribution, random-state initialization, precision, and
   interval limits should be supported?
6. How should the ABI represent weights and preserve useful precision through
   accumulation and integer-valued profile output?
7. What are the startup, rate-change, realloc, pause, detach, and heap-lifecycle
   semantics?
8. Should the existing coarse mode remain a separately described approximate
   mode, or is there a suitable statistical model for it?
9. What agreement is needed with the OTel USDT contract, particularly for
   estimator choice, byte-size meaning, and zero-size allocations?
10. Is an exact byte countdown cheap enough, or does a forward-scheduled
    size-class sampler justify extra free-list machinery?
11. What activation and snapshot boundaries are acceptable without a
    per-allocation atomic configuration check or process-wide rendezvous?

## References

- [jemalloc profiling internals](https://github.com/jemalloc/jemalloc/blob/dev/doc_internal/PROFILING_INTERNALS.md):
  inclusion-probability weighting and correcting samples before aggregation.
- [TCMalloc sampling](https://github.com/google/tcmalloc/blob/master/docs/sampling.md#weighting):
  randomized distances, mean-plus-overshoot weights, and requested-size-plus-one
  accounting.
- [OTel memory-profiling proposal](https://github.com/open-telemetry/opentelemetry-ebpf-profiler/blob/main/design-docs/00003-memory-profiling/README.md):
  the sampled allocation/free USDT contract.
- [Inspected mimalloc hook API](https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/include/mimalloc-profile.h).
- [Inspected mimalloc sampling path](https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/src/alloc.c#L963-L1006).
- [Inspected mimalloc coarse accounting](https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/src/page.c#L154-L175).

[mi-layout]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/include/mimalloc/types.h#L596-L682
[mi-fast]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/src/alloc.c#L50-L171
[mi-page-stats]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/src/page.c#L146-L240
[mi-collect]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/src/theap.c#L97-L163
[mi-detach]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/src/theap.c#L423-L449
[mi-free]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/src/free.c#L130-L178
[mi-page-lookup]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/include/mimalloc/internal.h#L669-L681
[mi-theap-init]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/src/theap.c#L269-L305
[mi-rng]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/src/random.c#L125-L154
[mi-refill]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/src/page.c#L309-L359
[mi-extend]: https://github.com/microsoft/mimalloc/blob/eaaed425687ba8fabd215b350f7e24f6d7cf6317/src/page.c#L650-L737
[je-unbias]: https://github.com/jemalloc/jemalloc/blob/dev/src/prof_data.c
