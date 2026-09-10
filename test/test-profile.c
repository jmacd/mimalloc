/* ----------------------------------------------------------------------------
Copyright (c) 2026-2026, Microsoft Research, Daniel Schwartz-Narbonne, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

// Tests for the mimalloc heap profiler (src/sample-profile.c).

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>

#include "mimalloc.h"
#include "mimalloc-profile.h"
#include "mimalloc/internal.h"
#include "mimalloc/prim.h"
#include "testhelper.h"

// ---------------------------------------------------------------------------
// Shared callback state (not thread safe!)
// ---------------------------------------------------------------------------

typedef struct {
  uint64_t  alloc_count;
  uint64_t  free_count;
  size_t    last_size;
  uint64_t  last_upscaled;
  void*     last_ptr;
  size_t    collect_count;
  uintptr_t collect_xor;
  size_t    collect_bytes;
  size_t    collect_data_size;
  const mi_heap_t* collect_heap;
  bool      collect_valid;
} profile_state_t;

// We store ptr in user_data so on_free can verify the round-trip.

static profile_state_t g_state;

#define TEST_THRESHOLD (16 * 1024)

static size_t mi_cdecl on_alloc(mi_profiler_data_t* data, void* ptr, size_t threshold, uint64_t bytes_since_last_sample, const mi_heap_t* heap, void* profiler_arg) {
  MI_UNUSED(threshold); MI_UNUSED(heap); MI_UNUSED(profiler_arg);
  assert(profiler_arg==&g_state);
  assert(bytes_since_last_sample >= data->requested_size);
  g_state.alloc_count++;
  g_state.last_ptr      = ptr;
  g_state.last_size     = data->requested_size;
  g_state.last_upscaled = bytes_since_last_sample;   
  // store ptr to verify round-trip
  data->user_data[0] = ptr; 
  for (size_t i = sizeof(mi_profiler_data_t); i < data->profiler_data_size; i++) {
    ((uint8_t*)data)[i] = (uint8_t)i;
  }
  return TEST_THRESHOLD;
}

static void mi_cdecl on_free(mi_profiler_data_t* data, void* ptr, const mi_heap_t* heap, void* profiler_arg) {
  MI_UNUSED(heap); MI_UNUSED_RELEASE(profiler_arg); MI_UNUSED_RELEASE(data);
  assert(profiler_arg==&g_state);
  g_state.free_count++;
  // verify the user_data round-trip
  assert(data->user_data[0] == ptr);
  if (g_state.last_ptr == ptr) { assert(data->requested_size == g_state.last_size); }
}

mi_profiler_t my_profiler = {
  NULL, NULL, NULL,  // reserved
  &g_state,          // profiler_arg
  sizeof(mi_profiler_data_t), // includes user_data[0]
  &on_alloc,       
  &on_free,
  NULL,
  NULL
};

static void mi_cdecl on_free_collect(mi_profiler_data_t* data, const mi_heap_t* heap, void* arg) {
  g_state.collect_count++;
  g_state.collect_xor ^= (uintptr_t)data->user_data[0];
  g_state.collect_bytes += data->requested_size;
  g_state.collect_valid = g_state.collect_valid && arg == &g_state && heap == g_state.collect_heap
                         && data->profiler_data_size == g_state.collect_data_size;
  for (size_t i = sizeof(mi_profiler_data_t); i < data->profiler_data_size; i++) {
    g_state.collect_valid = g_state.collect_valid && ((const uint8_t*)data)[i] == (uint8_t)i;
  }
}

static mi_profiler_t collect_profiler = {
  NULL, NULL, NULL,
  &g_state,
  sizeof(mi_profiler_data_t),
  &on_alloc,
  NULL,
  NULL,
  &on_free_collect
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Force at least one sample by allocating well over the threshold.
static void allocate_past_threshold(void) {
  size_t total = 0;
  while (total < TEST_THRESHOLD * 10) {
    void* p = mi_malloc(4096);
    mi_free(p);
    total += 4096;
    // mi_collect()
  }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

bool test_profiler_samples(void) {
  CHECK_BODY("profiler: on_alloc called after threshold") {
    uint64_t before = g_state.alloc_count;
    allocate_past_threshold();
    result = (g_state.alloc_count > before);
  }
  return true;
}

#define MAXLOOP 100000

bool test_profiler_record_fields(void) {
  CHECK_BODY("profiler: record ptr and size are non-zero") {
    uint64_t before = g_state.alloc_count;
    int count;
    for (count = 0; g_state.alloc_count == before && count < MAXLOOP; count++) {
      void* p = mi_malloc(1024);
      mi_free(p);
    }
    assert(count!=MAXLOOP);    
    result = (g_state.last_ptr != NULL && g_state.last_size > 0 && g_state.last_upscaled > 0 && count!=MAXLOOP);
  }
  return true;
}

bool test_profiler_on_free_called(void) {
  CHECK_BODY("profiler: on_free called for sampled allocation") {
    uint64_t alloc_before = g_state.alloc_count;
    uint64_t free_before  = g_state.free_count;

    // Keep the pointer live until we confirm a sample was taken, then free it.
    void* sampled = NULL;
    int count;
    for (count = 0; g_state.alloc_count == alloc_before && count < MAXLOOP; count++) {
      if (sampled) { mi_free(sampled); }
      sampled = mi_malloc(1024);
    }
    // At this point g_state.last_ptr is the sampled pointer.
    // Free it and check on_free fires.
    void* expected = g_state.last_ptr;
    mi_free(expected);
    sampled = NULL;
    assert(count!=MAXLOOP);
    result = (g_state.free_count > free_before && count!=MAXLOOP);
  }
  return true;
}

bool test_profiler_upscaled_at_least_size(void) {
  CHECK_BODY("profiler: upscaled_size >= size") {
    uint64_t before = g_state.alloc_count;
    int count;
    for (count = 0; g_state.alloc_count == before && count < MAXLOOP; count++) {
      void* p = mi_malloc(256);
      mi_free(p);
    }
    assert(count!=MAXLOOP);
    result = (g_state.last_upscaled >= g_state.last_size && count!=MAXLOOP);
  }
  return true;
}

bool test_profiler_free_count_le_alloc_count(void) {
  CHECK_BODY("profiler: on_free never called more times than on_alloc") {
    // Free can only fire for sampled allocations, so free_count <= alloc_count
    // must hold at all times.
    allocate_past_threshold();
    result = (g_state.free_count <= g_state.alloc_count);
  }
  return true;
}

static mi_heap_t* collect_heap_new(bool immediate) {
  mi_heap_t* heap = mi_heap_new();
  if (heap == NULL) return NULL;
  collect_profiler.on_free = (immediate ? &on_free : NULL);
  g_state.collect_count = 0;
  g_state.collect_xor = 0;
  g_state.collect_bytes = 0;
  g_state.collect_data_size = collect_profiler.profiler_data_size;
  g_state.collect_heap = heap;
  g_state.collect_valid = true;
  mi_heap_profile(heap, &collect_profiler);
  mi_profiler_start(&collect_profiler);
  return heap;
}

static void collect_heap_done(mi_heap_t* heap) {
  mi_heap_collect(heap, true);
  mi_heap_delete(heap);
  mi_profiler_stop(&collect_profiler);
}

static void* collect_sample(mi_heap_t* heap, size_t size) {
  mi_theap_t* theap = mi_heap_theap(heap);
  _mi_theap_set_profile_sample_rate(theap, TEST_THRESHOLD);
  theap->sample_countdown = 0;
  theap->profile_sample_countdown = 0;
  return mi_heap_malloc(heap, size);
}

static void* remote_ptrs[16];
static size_t remote_count;
static _Atomic(size_t) collect_ready;
static _Atomic(size_t) collect_go;

static bool free_collect_samples(void) {
  for (size_t i = 0; i < remote_count; i++) {
    mi_free(remote_ptrs[i]);
    remote_ptrs[i] = NULL;
  }
  return true;
}

static void collect_remote_cleanup(void) {
  for (size_t i = 0; i < remote_count; i++) {
    mi_free(remote_ptrs[i]);
    remote_ptrs[i] = NULL;
  }
}

static bool free_collect_even(void) {
  mi_atomic_increment_acq_rel(&collect_ready);
  while (mi_atomic_load_acquire(&collect_go) == 0) { _mi_prim_thread_yield(); }
  for (size_t i = 0; i < remote_count; i += 2) {
    mi_free(remote_ptrs[i]);
    remote_ptrs[i] = NULL;
  }
  return true;
}

static bool free_collect_odd(void) {
  mi_atomic_increment_acq_rel(&collect_ready);
  while (mi_atomic_load_acquire(&collect_go) == 0) { _mi_prim_thread_yield(); }
  for (size_t i = 1; i < remote_count; i += 2) {
    mi_free(remote_ptrs[i]);
    remote_ptrs[i] = NULL;
  }
  return true;
}

static void collect_release_threads(bool both_started) {
  if (both_started) {
    while (mi_atomic_load_acquire(&collect_ready) != 2) { _mi_prim_thread_yield(); }
  }
  mi_atomic_store_release(&collect_go, 1);
}

static bool free_collect_concurrent(void) {
  mi_atomic_store_release(&collect_ready, 0);
  mi_atomic_store_release(&collect_go, 0);
  mi_thread_fun_args_t args[2] = { { &free_collect_even, false }, { &free_collect_odd, false } };
  #ifdef _WIN32
  HANDLE threads[2];
  for (size_t i = 0; i < 2; i++) {
    threads[i] = CreateThread(NULL, 0, &mi_win_thread_entry, &args[i], 0, NULL);
  }
  collect_release_threads(threads[0] != NULL && threads[1] != NULL);
  for (size_t i = 0; i < 2; i++) {
    if (threads[i] != NULL) {
      WaitForSingleObject(threads[i], INFINITE);
      CloseHandle(threads[i]);
    }
  }
  #else
  pthread_t threads[2];
  bool started[2];
  for (size_t i = 0; i < 2; i++) {
    started[i] = (pthread_create(&threads[i], NULL, &mi_pthread_entry, &args[i]) == 0);
  }
  collect_release_threads(started[0] && started[1]);
  for (size_t i = 0; i < 2; i++) {
    if (started[i]) { pthread_join(threads[i], NULL); }
  }
  #endif
  return (args[0].result && args[1].result);
}

bool test_profiler_collect_local(void) {
  CHECK_BODY("profiler: local collection preserves the immediate callback") {
    mi_heap_t* heap = collect_heap_new(true);
    if (heap == NULL) { result = false; }
    else {
      const size_t sizes[] = { 128, 2*1024*1024 };
      for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        void* p = collect_sample(heap, sizes[i]);
        if (p == NULL) { result = false; break; }
        memset(p, 0x5A, sizes[i]);
        const uintptr_t expected = g_state.collect_xor ^ (uintptr_t)p;
        const uint64_t frees = g_state.free_count;
        mi_free(p);
        result = (g_state.free_count == frees + 1 && g_state.collect_count == i + 1
                  && g_state.collect_xor == expected && g_state.collect_valid) && result;
        mi_heap_collect(heap, true);
        result = (g_state.collect_count == i + 1) && result;
      }
      collect_heap_done(heap);
    }
  }
  return true;
}

bool test_profiler_collect_remote(bool stop) {
  CHECK_BODY(stop ? "profiler: stopped collection still accounts for every block"
                  : "profiler: mixed remote list preserves samples and exact used count") {
    mi_heap_t* heap = collect_heap_new(false);
    if (heap == NULL) { result = false; }
    else {
      void* a = collect_sample(heap, 128);
      if (a == NULL) { result = false; }
      else {
        mi_page_t* page = _mi_ptr_page(a);
        const size_t bsize = mi_page_usable_block_size(page);
        void* b = mi_heap_malloc(heap, bsize);
        void* c = collect_sample(heap, 128);
        void* anchor = mi_heap_malloc(heap, bsize);
        remote_ptrs[0] = a; remote_ptrs[1] = b; remote_ptrs[2] = c;
        remote_count = 3;
        if (b == NULL || c == NULL || anchor == NULL) { result = false; }
        else {
          result = (_mi_ptr_page(b) == page && _mi_ptr_page(c) == page
                    && _mi_ptr_page(anchor) == page && mi_page_used(page) == 4) && result;
          memset(a, 0x5A, 128);
          memset(c, 0x5A, 128);
          result = mi_run_on_thread(&free_collect_samples) && result;
          result = (g_state.collect_count == 0 && mi_page_used(page) == 4) && result;
          #if (MI_DEBUG>0) && !MI_TRACK_ENABLED && !MI_TSAN
          for (size_t i = 0; i < 128; i++) {
            result = (((const uint8_t*)a)[i] == MI_DEBUG_FREED
                      && ((const uint8_t*)c)[i] == MI_DEBUG_FREED) && result;
          }
          #endif
          if (stop) { mi_profiler_stop(&collect_profiler); }
          _mi_page_free_collect(page, true);
          result = (mi_page_used(page) == 1 && g_state.collect_count == (stop ? 0 : 2)) && result;
          if (!stop) {
            result = (g_state.collect_xor == ((uintptr_t)a ^ (uintptr_t)c)
                      && g_state.collect_bytes == 256 && g_state.collect_valid) && result;
          }
          mi_profiler_start(&collect_profiler);
          _mi_page_free_collect(page, true);
          void* reused = mi_heap_malloc(heap, bsize);
          result = (reused != NULL) && result;
          mi_free(reused);
          result = (g_state.collect_count == (stop ? 0 : 2)) && result;
        }
        collect_remote_cleanup();
        mi_free(anchor);
      }
      collect_heap_done(heap);
    }
  }
  return true;
}

bool test_profiler_collect_partial(void) {
  CHECK_BODY("profiler: partial collection preserves the pending head") {
    mi_heap_t* heap = collect_heap_new(false);
    if (heap == NULL) { result = false; }
    else {
      void* a = collect_sample(heap, 128);
      if (a == NULL) { result = false; }
      else {
        mi_page_t* page = _mi_ptr_page(a);
        void* b = collect_sample(heap, 128);
        void* anchor = mi_heap_malloc(heap, mi_page_usable_block_size(page));
        remote_ptrs[0] = b; remote_ptrs[1] = a;
        remote_count = 2;
        if (b == NULL || anchor == NULL) { result = false; }
        else {
          mi_block_t* head = _mi_page_ptr_unalign(page, a);
          result = mi_run_on_thread(&free_collect_samples) && result;
          result = (_mi_page_free_collect_partly(page, head) == head
                    && mi_page_used(page) == 2 && g_state.collect_count == 1
                    && g_state.collect_xor == (uintptr_t)b) && result;
          bool pending = false;
          result = (mi_block_next_mt(page, head, &pending) == NULL && pending) && result;
          _mi_page_free_collect(page, true);
          result = (mi_page_used(page) == 1 && g_state.collect_count == 2
                    && g_state.collect_xor == ((uintptr_t)a ^ (uintptr_t)b)
                    && g_state.collect_valid) && result;
        }
        collect_remote_cleanup();
        mi_free(anchor);
      }
      collect_heap_done(heap);
    }
  }
  return true;
}

bool test_profiler_collect_concurrent(void) {
  CHECK_BODY("profiler: concurrent native producers preserve mixed sample markers") {
    mi_heap_t* heap = collect_heap_new(false);
    if (heap == NULL) { result = false; }
    else {
      void* first = collect_sample(heap, 128);
      if (first == NULL) { result = false; }
      else {
        mi_page_t* page = _mi_ptr_page(first);
        const size_t bsize = mi_page_usable_block_size(page);
        remote_count = 16;
        remote_ptrs[0] = first;
        uintptr_t expected = (uintptr_t)first;
        for (size_t i = 1; i < remote_count; i++) {
          remote_ptrs[i] = (i%2 == 0 ? collect_sample(heap, 128) : mi_heap_malloc(heap, bsize));
          if (i%2 == 0) { expected ^= (uintptr_t)remote_ptrs[i]; }
          result = (remote_ptrs[i] != NULL && _mi_ptr_page(remote_ptrs[i]) == page) && result;
        }
        void* anchor = mi_heap_malloc(heap, bsize);
        result = (anchor != NULL) && result;
        if (result) {
          result = free_collect_concurrent();
          result = (mi_page_used(page) == 17 && g_state.collect_count == 0) && result;
          _mi_page_free_collect(page, true);
          result = (mi_page_used(page) == 1 && g_state.collect_count == 8
                    && g_state.collect_xor == expected && g_state.collect_valid) && result;
        }
        collect_remote_cleanup();
        mi_free(anchor);
      }
      collect_heap_done(heap);
    }
  }
  return true;
}

static bool allocate_collect_samples(void) {
  mi_heap_t* heap = (mi_heap_t*)g_state.collect_heap;
  remote_ptrs[0] = collect_sample(heap, 128);
  remote_ptrs[1] = NULL;
  if (remote_ptrs[0] == NULL) return false;
  remote_ptrs[1] = mi_heap_malloc(heap, mi_page_usable_block_size(_mi_ptr_page(remote_ptrs[0])));
  return (remote_ptrs[1] != NULL);
}

bool test_profiler_collect_abandoned(void) {
  CHECK_BODY("profiler: abandoned-page collection retains origin heap and cookie") {
    mi_heap_t* heap = collect_heap_new(false);
    if (heap == NULL) { result = false; }
    else {
      remote_count = 2;
      result = mi_run_on_thread(&allocate_collect_samples);
      const uintptr_t expected = (uintptr_t)remote_ptrs[0];
      collect_remote_cleanup();
      mi_heap_collect(heap, true);
      result = (g_state.collect_count == 1 && g_state.collect_xor == expected
                && g_state.collect_bytes == 128 && g_state.collect_valid) && result;
      collect_heap_done(heap);
    }
  }
  return true;
}

bool test_profiler_collect_extended(void) {
  CHECK_BODY("profiler: remote collection preserves an extended unaligned data prefix") {
    collect_profiler.profiler_data_size = sizeof(mi_profiler_data_t) + 17;
    mi_heap_t* heap = collect_heap_new(false);
    if (heap == NULL) { result = false; }
    else {
      void* p = collect_sample(heap, 128);
      if (p == NULL) { result = false; }
      else {
        mi_page_t* page = _mi_ptr_page(p);
        void* anchor = mi_heap_malloc(heap, mi_page_usable_block_size(page));
        remote_count = 1;
        remote_ptrs[0] = p;
        if (anchor == NULL) { result = false; }
        else {
          result = mi_run_on_thread(&free_collect_samples);
          _mi_page_free_collect(page, true);
          result = (g_state.collect_count == 1 && g_state.collect_xor == (uintptr_t)p
                    && g_state.collect_valid && mi_page_used(page) == 1) && result;
        }
        collect_remote_cleanup();
        mi_free(anchor);
      }
      collect_heap_done(heap);
    }
    collect_profiler.profiler_data_size = sizeof(mi_profiler_data_t);
  }
  return true;
}


// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(void) {
  mi_profile(&my_profiler);
  mi_profiler_start(&my_profiler);

  test_profiler_upscaled_at_least_size();
  test_profiler_samples();
  test_profiler_record_fields();
  test_profiler_on_free_called();
  test_profiler_free_count_le_alloc_count();

  mi_profiler_stop(&my_profiler);

  test_profiler_collect_local();
  test_profiler_collect_remote(false);
  test_profiler_collect_remote(true);
  test_profiler_collect_partial();
  test_profiler_collect_concurrent();
  test_profiler_collect_abandoned();
  test_profiler_collect_extended();

  return print_test_summary();
}
