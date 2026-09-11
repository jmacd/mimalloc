/* ----------------------------------------------------------------------------
Copyright (c) 2026, Microsoft Research
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#include "mimalloc.h"
#include "mimalloc-stats.h"
#include "mimalloc-profile.h"
#include "mimalloc/internal.h"
#include "testhelper.h"

#if MI_THREAD_STATS

static mi_heap_t* activity_heap_new(void) {
  mi_heap_t* heap = mi_heap_new();
  if (heap != NULL) {
    mi_theap_guarded_set_sample_rate(mi_heap_theap(heap), 0, 0);
  }
  return heap;
}

static void* remote_ptr;
static uint64_t remote_allocated;
static uint64_t remote_freed;

static bool activity_remote_free(void) {
  mi_thread_init();
  uint64_t allocated, freed;
  if (mi_thread_activity_get(&allocated, &freed) != 0) return false;
  mi_free(remote_ptr);
  remote_ptr = NULL;
  if (mi_thread_activity_get(&remote_allocated, &remote_freed) != 0) return false;
  remote_allocated -= allocated;
  remote_freed -= freed;
  return true;
}

static bool activity_remote_free_and_reset(void) {
  mi_thread_init();
  uint64_t allocated = 0, freed = 0;
  if (mi_thread_activity_get_and_reset(&allocated, &freed) != 0) return false;
  mi_free(remote_ptr);
  remote_ptr = NULL;
  if (mi_thread_activity_get_and_reset(&remote_allocated, &remote_freed) != 0) return false;
  return (mi_thread_activity_get_and_reset(&allocated, &freed) == 0 && allocated == 0 && freed == 0);
}

static size_t mi_cdecl activity_on_alloc(mi_profiler_data_t* data, void* p, size_t rate,
                                      uint64_t weight, const mi_heap_t* heap, void* arg) {
  MI_UNUSED(rate); MI_UNUSED(weight); MI_UNUSED(heap); MI_UNUSED(arg);
  data->user_data[0] = p;
  return 16*1024;
}

static size_t collected;

static void mi_cdecl activity_on_collect(mi_profiler_data_t* data, const mi_heap_t* heap, void* arg) {
  MI_UNUSED(data); MI_UNUSED(heap); MI_UNUSED(arg);
  collected++;
}

static mi_profiler_t profiler = {
  NULL, NULL, NULL, NULL, sizeof(mi_profiler_data_t),
  &activity_on_alloc, NULL, NULL, &activity_on_collect
};

static void* activity_malloc(mi_heap_t* heap, size_t size, bool sampled) {
  if (sampled) {
    mi_theap_t* theap = mi_heap_theap(heap);
    _mi_theap_set_profile_sample_rate(theap, 16*1024);
    theap->profile_sample_countdown = 0;
    theap->sample_countdown = 0;
  }
  return mi_heap_malloc(heap, size);
}

static bool mi_cdecl activity_inventory(const mi_heap_t* heap, const mi_heap_area_t* area,
                                      void* block, size_t size, void* arg) {
  MI_UNUSED(heap); MI_UNUSED(size);
  if (block == NULL) { *(size_t*)arg += area->used * area->full_block_size; }
  return true;
}

static bool test_activity_remote(bool sampled, bool reset) {
  CHECK_BODY(reset ? "activity: interval consumption credits a remote free to its caller"
                  : (sampled ? "activity: sampled remote free credits caller, not collector"
                             : "activity: remote free credits caller, inventory credits origin")) {
    mi_heap_t* heap = activity_heap_new();
    if (heap == NULL) { result = false; }
    else {
      if (sampled) {
        mi_heap_profile(heap, &profiler);
        mi_profiler_start(&profiler);
      }
      void* anchor = activity_malloc(heap, 128, sampled);
      if (anchor == NULL) { result = false; }
      else {
        uint64_t a0 = 0, f0 = 0, a1 = 0, f1 = 0;
        result = (mi_thread_activity_get(&a0, &f0) == 0);
        void* p = activity_malloc(heap, 128, sampled);
        if (p == NULL) { result = false; }
        else {
          mi_page_t* page = _mi_ptr_page(p);
          const size_t bytes = mi_page_block_size(page);
          result = (_mi_ptr_page(anchor) == page && mi_page_used(page) == 2) && result;
          result = (mi_thread_activity_get(&a1, &f1) == 0 && a1 - a0 == bytes && f1 == f0) && result;
          remote_ptr = p;
          collected = 0;
          result = mi_run_on_thread(reset ? &activity_remote_free_and_reset : &activity_remote_free) && result;
          result = (remote_allocated == 0 && remote_freed == bytes && mi_page_used(page) == 2) && result;
          _mi_page_free_collect(page, true);
          _mi_page_update_stats(page);
          size_t live = 0;
          result = mi_heap_visit_blocks(heap, false, &activity_inventory, &live) && result;
          result = (live == bytes && mi_page_used(page) == 1 && collected == (sampled ? 1 : 0)) && result;
          mi_theap_stats_merge_to_heap(mi_heap_theap(heap));
          mi_stats_t_decl(stats);
          result = mi_heap_stats_get(heap, &stats) && result;
          result = (mi_thread_activity_get(&a1, &f1) == 0 && a1 - a0 == bytes && f1 == f0) && result;
          mi_free(remote_ptr);
          remote_ptr = NULL;
        }
        mi_free(anchor);
      }
      mi_heap_delete(heap);
      if (sampled) { mi_profiler_stop(&profiler); }
    }
  }
  return true;
}

static bool test_activity_heaps(void) {
  CHECK_BODY("activity: counters span heaps and survive statistics merges") {
    mi_heap_t* heap = activity_heap_new();
    mi_heap_t* other = activity_heap_new();
    if (heap == NULL || other == NULL) { result = false; }
    else {
      void* anchor = mi_heap_malloc(heap, 64);
      void* other_anchor = mi_heap_malloc(other, 64);
      if (anchor == NULL || other_anchor == NULL) { result = false; }
      else {
        uint64_t a0 = 0, f0 = 0, a1 = 0, f1 = 0;
        result = (mi_thread_activity_get(&a0, &f0) == 0);
        mi_theap_t* previous = mi_theap_set_default(mi_heap_theap(heap));
        void* p = mi_malloc(64);
        mi_theap_set_default(mi_heap_theap(other));
        void* q = mi_malloc(64);
        mi_theap_set_default(previous);
        if (p == NULL || q == NULL) { result = false; }
        else {
          const uint64_t bytes = mi_page_block_size(_mi_ptr_page(p)) + mi_page_block_size(_mi_ptr_page(q));
          result = (mi_thread_activity_get(&a1, &f1) == 0 && a1 - a0 == bytes && f1 == f0) && result;
          mi_free(p); p = NULL;
          mi_free(q); q = NULL;
          mi_theap_stats_merge_to_heap(mi_heap_theap(heap));
          mi_theap_stats_merge_to_heap(mi_heap_theap(other));
          mi_stats_reset();
          result = (mi_thread_activity_get(&a1, &f1) == 0 && a1 - a0 == bytes && f1 - f0 == bytes) && result;
          uint64_t a2, f2;
          result = (mi_thread_activity_get(&a2, &f2) == 0 && a2 == a1 && f2 == f1) && result;
        }
        mi_free(p); mi_free(q);
      }
      mi_free(anchor); mi_free(other_anchor);
    }
    mi_heap_delete(other); mi_heap_delete(heap);
  }
  return true;
}

static bool test_activity_sizes(void) {
  CHECK_BODY("activity: zero-size and aligned frees charge the full native block") {
    mi_heap_t* heap = activity_heap_new();
    if (heap == NULL) { result = false; }
    else {
      for (int aligned = 0; aligned < 2; aligned++) {
        void* p = (aligned ? mi_heap_malloc_aligned(heap, 100, 64) : mi_heap_malloc(heap, 0));
        if (p == NULL) { result = false; break; }
        mi_page_t* page = _mi_ptr_page(p);
        const size_t bytes = mi_page_block_size(page);
        void* anchor = mi_heap_malloc(heap, mi_page_usable_block_size(page));
        if (anchor == NULL) { result = false; }
        else {
          uint64_t a0 = 0, f0 = 0, a1 = 0, f1 = 0;
          result = (_mi_ptr_page(anchor) == page && mi_thread_activity_get(&a0, &f0) == 0) && result;
          mi_free(p); p = NULL;
          result = (mi_thread_activity_get(&a1, &f1) == 0 && a1 == a0 && f1 - f0 == bytes) && result;
        }
        mi_free(p); mi_free(anchor);
      }
      mi_heap_delete(heap);
    }
  }
  CHECK_BODY("activity: in-place resize is not a block allocation or free") {
    void* p = mi_malloc(256);
    if (p == NULL) { result = false; }
    else {
      uint64_t a0 = 0, f0 = 0, a1 = 0, f1 = 0;
      result = (mi_thread_activity_get(&a0, &f0) == 0);
      result = (mi_expand(p, 128) == p) && result;
      result = (mi_thread_activity_get(&a1, &f1) == 0 && a1 == a0 && f1 == f0) && result;
      mi_free(p);
    }
  }
  return true;
}

#if MI_GUARDED
static bool test_activity_guarded(void) {
  CHECK_BODY("activity: guarded frees charge the complete native block extent") {
    mi_heap_t* heap = activity_heap_new();
    if (heap == NULL) { result = false; }
    else {
      mi_theap_t* theap = mi_heap_theap(heap);
      mi_theap_guarded_set_sample_rate(theap, 16*1024, 1);
      mi_theap_guarded_set_size_bound(theap, 0, SIZE_MAX);
      theap->guarded_sample_countdown = 0;
      theap->sample_countdown = 0;
      void* p = mi_heap_malloc(heap, 128);
      mi_theap_guarded_set_sample_rate(theap, 0, 0);
      if (p == NULL) { result = false; }
      else {
        mi_page_t* page = _mi_ptr_page(p);
        result = mi_block_ptr_is_guarded(_mi_page_ptr_unalign(page, p), p) && result;
        const size_t bytes = mi_page_block_size(page);
        void* anchor = mi_heap_malloc(heap, mi_page_usable_block_size(page));
        if (anchor == NULL) { result = false; }
        else {
          uint64_t a0 = 0, f0 = 0, a1 = 0, f1 = 0;
          result = (_mi_ptr_page(anchor) == page && mi_thread_activity_get(&a0, &f0) == 0) && result;
          mi_free(p); p = NULL;
          result = (mi_thread_activity_get(&a1, &f1) == 0 && a1 == a0 && f1 - f0 == bytes) && result;
        }
        mi_free(p); mi_free(anchor);
      }
      mi_heap_delete(heap);
    }
  }
  return true;
}
#endif

static bool activity_window(void) {
  mi_thread_done();
  uint64_t allocated = 123, freed = 456;
  bool result = (mi_thread_activity_get(&allocated, &freed) == EAGAIN && allocated == 123 && freed == 456);
  result = (mi_thread_activity_get_and_reset(&allocated, &freed) == EAGAIN && allocated == 123 && freed == 456) && result;
  mi_free(remote_ptr);
  remote_ptr = NULL;
  result = (mi_thread_activity_get(&allocated, &freed) == EAGAIN) && result;
  mi_thread_init();
  result = (mi_thread_activity_get(&allocated, &freed) == 0) && result;
  uint64_t allocated2 = 0, freed2 = 0;
  result = (mi_thread_activity_get(&allocated2, &freed2) == 0 && allocated2 == allocated && freed2 == freed) && result;
  return result;
}

static bool test_activity_window(void) {
  CHECK_BODY("activity: uninitialized free does not create a misleading observation window") {
    void* anchor = mi_malloc(64);
    remote_ptr = mi_malloc(64);
    if (anchor == NULL || remote_ptr == NULL) { result = false; }
    else { result = mi_run_on_thread(&activity_window); }
    mi_free(remote_ptr);
    remote_ptr = NULL;
    mi_free(anchor);
  }
  return true;
}

static bool test_activity_get_and_reset(void) {
  CHECK_BODY("activity: explicit consumption returns exact intervals without changing inventory") {
    mi_heap_t* heap = activity_heap_new();
    if (heap == NULL) { result = false; }
    else {
      void* anchor = mi_heap_malloc(heap, 64);
      if (anchor == NULL) { result = false; }
      else {
        uint64_t allocated = 0, freed = 0;
        result = (mi_thread_activity_get_and_reset(&allocated, &freed) == 0);
        void* p = mi_heap_malloc(heap, 64);
        if (p == NULL) { result = false; }
        else {
          mi_page_t* page = _mi_ptr_page(p);
          const size_t bytes = mi_page_block_size(page);
          freed = 456;
          result = (mi_thread_activity_get_and_reset(NULL, &freed) == EINVAL && freed == 456) && result;
          result = (mi_thread_activity_get(&allocated, &freed) == 0 && allocated == bytes && freed == 0) && result;
          result = (mi_thread_activity_get_and_reset(&allocated, &freed) == 0 && allocated == bytes && freed == 0) && result;
          result = (mi_thread_activity_get_and_reset(&allocated, &freed) == 0 && allocated == 0 && freed == 0
                    && mi_page_used(page) == 2) && result;
          mi_free(p);
          result = (mi_thread_activity_get_and_reset(&allocated, &freed) == 0 && allocated == 0 && freed == bytes) && result;
          result = (mi_thread_activity_get(&allocated, &freed) == 0 && allocated == 0 && freed == 0
                    && mi_page_used(page) == 1) && result;
        }
        mi_free(anchor);
      }
      mi_heap_delete(heap);
    }
  }
  CHECK_BODY("activity: consuming an overflowed interval reports loss and restores collection") {
    mi_tld_t* tld = mi_theap_get_default()->tld;
    for (int counter = 0; counter < 2; counter++) {
      tld->activity_allocated = (counter == 0 ? UINT64_MAX : 7);
      tld->activity_freed = (counter == 1 ? UINT64_MAX : 7);
      uint64_t allocated = 123, freed = 456;
      result = (mi_thread_activity_get_and_reset(NULL, &freed) == EINVAL && freed == 456) && result;
      result = (mi_thread_activity_get(&allocated, &freed) == EOVERFLOW && allocated == 123 && freed == 456) && result;
      result = (mi_thread_activity_get_and_reset(&allocated, &freed) == EOVERFLOW
                && allocated == 123 && freed == 456) && result;
      result = (mi_thread_activity_get(&allocated, &freed) == 0 && allocated == 0 && freed == 0) && result;
    }
  }
  return true;
}

static bool test_activity_errors(void) {
  CHECK_BODY("activity: failed allocation, null free and reads do not add block activity") {
    uint64_t a0 = 0, f0 = 0, a1 = 0, f1 = 0;
    result = (mi_thread_activity_get(&a0, &f0) == 0);
    volatile size_t too_large = SIZE_MAX;
    void* p = mi_malloc(too_large);
    mi_free(p);
    result = (p == NULL && mi_thread_activity_get(&a1, &f1) == 0 && a1 == a0 && f1 == f0) && result;
  }
  CHECK_BODY("activity: overflow is explicit and outputs remain unchanged") {
    mi_tld_t* tld = mi_theap_get_default()->tld;
    const uint64_t saved_allocated = tld->activity_allocated;
    const uint64_t saved_freed = tld->activity_freed;
    tld->activity_allocated = UINT64_MAX - 1;
    void* p = mi_malloc(16);
    uint64_t allocated = 123, freed = 456;
    result = (p != NULL && mi_thread_activity_get(&allocated, &freed) == EOVERFLOW
              && allocated == 123 && freed == 456);
    mi_free(p);
    tld->activity_allocated = saved_allocated;
    tld->activity_freed = saved_freed;
    p = mi_malloc(16);
    tld->activity_freed = UINT64_MAX - 1;
    mi_free(p);
    result = (p != NULL && mi_thread_activity_get(&allocated, &freed) == EOVERFLOW
              && allocated == 123 && freed == 456) && result;
    tld->activity_allocated = saved_allocated;
    tld->activity_freed = saved_freed;
    result = (mi_thread_activity_get(NULL, &freed) == EINVAL && freed == 456) && result;
  }
  return true;
}

#endif

int main(void) {
  mi_thread_init();
  #if MI_THREAD_STATS
  mi_theap_guarded_set_sample_rate(mi_theap_get_default(), 0, 0);
  test_activity_heaps();
  test_activity_sizes();
  test_activity_remote(false, false);
  test_activity_remote(false, true);
  #if MI_PROFILE
  test_activity_remote(true, false);
  #endif
  test_activity_window();
  test_activity_get_and_reset();
  #if MI_GUARDED
  test_activity_guarded();
  #endif
  test_activity_errors();
  #else
  uint64_t allocated = 123, freed = 456;
  CHECK("activity: unavailable build leaves outputs unchanged",
        mi_thread_activity_get(&allocated, &freed) == ENOSYS && allocated == 123 && freed == 456);
  CHECK("activity: unavailable consumption leaves outputs unchanged",
        mi_thread_activity_get_and_reset(&allocated, &freed) == ENOSYS && allocated == 123 && freed == 456);
  #endif
  return print_test_summary();
}
