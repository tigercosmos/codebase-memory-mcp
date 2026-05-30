/*
 * cbm_atomic.h — Portable atomic typedefs across C11 and C++23.
 *
 * The original C code declares shared counters as `_Atomic int64_t`,
 * etc., and pulls in `<stdatomic.h>` for atomic_load / atomic_store /
 * atomic_fetch_add. Under C++23 the `_Atomic` keyword does not exist
 * and `<stdatomic.h>` is not portable; the equivalent is
 * `std::atomic<T>` with the free functions `std::atomic_load` etc.
 *
 * To keep the same header includable from both C tests and C++ libs
 * (which is required because src/pipeline/pipeline_internal.h and
 * src/graph_buffer/graph_buffer.h appear in both translation units),
 * this header provides cross-language typedefs whose memory layout
 * matches between C `_Atomic T` and C++ `std::atomic<T>` (same size,
 * same alignment, same operation semantics under libstdc++/glibc).
 *
 * Usage:
 *   #include "foundation/cbm_atomic.h"
 *   cbm_atomic_int64 counter;
 *   atomic_store(&counter, 0);          // C: macro, C++: std::atomic_store
 *   int64_t v = atomic_load(&counter);
 */
#ifndef CBM_ATOMIC_H
#define CBM_ATOMIC_H

#include <stdint.h>

#ifdef __cplusplus

#include <atomic>

/* Hoist the C11 typedef names from std into ::, matching <stdatomic.h>'s
 * global scope so existing call sites work without qualification. */
using std::atomic_bool;
using std::atomic_int;
using std::atomic_llong;
using std::atomic_long;
using std::atomic_uint;
using std::atomic_ullong;
using std::atomic_ulong;

using cbm_atomic_int = std::atomic<int>;
using cbm_atomic_int32 = std::atomic<int32_t>;
using cbm_atomic_int64 = std::atomic<int64_t>;
using cbm_atomic_uint32 = std::atomic<uint32_t>;
using cbm_atomic_uint64 = std::atomic<uint64_t>;
using cbm_atomic_bool = std::atomic<bool>;
using cbm_atomic_size = std::atomic<size_t>;

/* Bring std::atomic_* into the global namespace so existing C-style
 * call sites (atomic_load, atomic_store, ...) continue to compile.
 * `<atomic>` already declares atomic_init, atomic_load, atomic_store,
 * atomic_fetch_add, atomic_fetch_sub, atomic_exchange, atomic_thread_fence
 * inside namespace std as free function templates over std::atomic<T>. */
using std::atomic_compare_exchange_strong;
using std::atomic_compare_exchange_weak;
using std::atomic_exchange;
using std::atomic_exchange_explicit;
using std::atomic_fetch_add;
using std::atomic_fetch_add_explicit;
using std::atomic_fetch_sub;
using std::atomic_fetch_sub_explicit;
using std::atomic_init;
using std::atomic_load;
using std::atomic_load_explicit;
using std::atomic_signal_fence;
using std::atomic_store;
using std::atomic_store_explicit;
using std::atomic_thread_fence;

/* memory_order constants. */
using std::memory_order;
using std::memory_order_acq_rel;
using std::memory_order_acquire;
using std::memory_order_consume;
using std::memory_order_relaxed;
using std::memory_order_release;
using std::memory_order_seq_cst;

#else

#include <stdatomic.h>

typedef _Atomic int cbm_atomic_int;
typedef _Atomic int32_t cbm_atomic_int32;
typedef _Atomic int64_t cbm_atomic_int64;
typedef _Atomic uint32_t cbm_atomic_uint32;
typedef _Atomic uint64_t cbm_atomic_uint64;
typedef _Atomic _Bool cbm_atomic_bool;
typedef _Atomic size_t cbm_atomic_size;

#endif /* __cplusplus */

#endif /* CBM_ATOMIC_H */
