/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What the compilers disagree about: GCC (the tree's default), Clang and MSVC.
 *
 * The MSVC branches are written but have never been compiled -- the cross
 * toolchain is mingw-w64, which is GCC.
 */

/* Pinned here because this is the lowest header in the tree and the first to
 * pull in libc: glibc hides pthread_condattr_setclock and CLOCK_MONOTONIC
 * behind it, and a feature-test macro set after the first libc include is set
 * too late. It is why app.h, which includes this first, leads every .c. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
/* And on macOS everything beside POSIX as well: the system's headers hide
 * whatever POSIX does not name once _POSIX_C_SOURCE is set, which is half
 * of what AppKit's own headers include and pthread_cond_timedwait_relative_np
 * besides (core/thread.h). */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

#ifndef FNL_COMPILER_H
#define FNL_COMPILER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#if defined(_WIN32)
#  include <malloc.h>   /* _aligned_malloc / _aligned_free */
#endif

/* ---- attributes ---------------------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
/* A dropped result is the failure mode the two error tiers exist to prevent:
 * an abort cannot be ignored and a return value can. */
#  define FNL_MUST_USE   __attribute__((warn_unused_result))
/* gnu_printf on mingw: GCC targeting Windows checks a format against
 * Microsoft's pre-C99 runtime, which has no %zu, while the build links
 * mingw's own printf (__USE_MINGW_ANSI_STDIO, set in CMakeLists.txt). */
#  if defined(__MINGW32__)
#    define FNL_PRINTF(f, a) __attribute__((format(gnu_printf, f, a)))
#  else
#    define FNL_PRINTF(f, a) __attribute__((format(printf, f, a)))
#  endif
#  define FNL_THREAD_LOCAL _Thread_local
#  define FNL_NORETURN     __attribute__((noreturn))
/* Every pointer argument is valid. For an internal entry that inlines the
 * public ones: each of those tests its FnlApp for NULL, and under LTO gcc
 * splits off a path on which it was, then warns about a store through it
 * (-Wstringop-overflow, "likely at address zero"). */
#  define FNL_NONNULL      __attribute__((nonnull))
#elif defined(_MSC_VER)
/* Only enforced under /analyze, so weaker there: GCC is the compiler that
 * polices this, MSVC the one that has to accept it. */
#  define FNL_MUST_USE   _Check_return_
#  define FNL_PRINTF(f, a)
/* MSVC takes _Thread_local only in recent C mode; this means the same for a
 * static, which is the only way it is used here. */
#  define FNL_THREAD_LOCAL __declspec(thread)
#  define FNL_NORETURN     __declspec(noreturn)
#  define FNL_NONNULL
#else
#  define FNL_MUST_USE
#  define FNL_PRINTF(f, a)
#  define FNL_THREAD_LOCAL _Thread_local
#  define FNL_NORETURN
#  define FNL_NONNULL
#endif

/* Clang's -Wthread-safety, which the clang trees build with. No-ops on every
 * other compiler, so the annotations are free where nothing reads them.
 * FnlMutex is a capability (core/thread.h), so every lock is checked for
 * being released on every path; a function that expects one held says so
 * with FNL_REQUIRES. Not guarded_by on App's fields: clang 21 does not bind
 * a member's guarded_by to its instance in C, and reports every access as
 * unlocked, the locked ones included. */
#if defined(__clang__)
#  define FNL_TSA(x) __attribute__((x))
#else
#  define FNL_TSA(x)
#endif
#define FNL_CAPABILITY(name)   FNL_TSA(capability(name))
#define FNL_REQUIRES(...)      FNL_TSA(requires_capability(__VA_ARGS__))
#define FNL_EXCLUDES(...)      FNL_TSA(locks_excluded(__VA_ARGS__))
#define FNL_ACQUIRE(...)       FNL_TSA(acquire_capability(__VA_ARGS__))
#define FNL_RELEASE(...)       FNL_TSA(release_capability(__VA_ARGS__))
#define FNL_NO_TSA             FNL_TSA(no_thread_safety_analysis)

/* MSVC accepts _Alignof only in its C11 mode. */
#if defined(_MSC_VER)
#  define FNL_ALIGNOF(T) __alignof(T)
#else
#  define FNL_ALIGNOF(T) _Alignof(T)
#endif

/* ---- aligned allocation --------------------------------------------------
 *
 * App carries _Alignas(64) members: one at the head of each run of fields
 * with its own writer -- the pump thread's, the render thread's, and what the
 * two share -- and one on the event queue's lock. Plain malloc guarantees
 * only max_align_t, and getting this wrong fails no build and no test: it
 * quietly reintroduces the false sharing those annotations prevent.
 *
 * Microsoft's runtime has no aligned_alloc, and its _aligned_malloc must be
 * paired with _aligned_free rather than free(). Guarded on _WIN32 rather than
 * _MSC_VER because mingw-w64 links the same runtime. The rule throughout this
 * header: where the two differ over a *library*, test the runtime; over a
 * *spelling*, test the compiler. */
static inline void *fnl_aligned_alloc(size_t align, size_t bytes)
{
#if defined(_WIN32)
	return _aligned_malloc(bytes, align);
#else
	/* C11 requires the size to be a multiple of the alignment. C17 relaxed it,
	 * but the rounding costs nothing and keeps this correct under both. */
	return aligned_alloc(align, (bytes + align - 1) / align * align);
#endif
}

static inline void fnl_aligned_free(void *p)
{
#if defined(_WIN32)
	_aligned_free(p);
#else
	free(p);
#endif
}

/* ---- atomics -------------------------------------------------------------
 *
 * Everything this library shares between threads without a lock is a plain
 * load or store of a 32-bit scalar or a pointer, and fetch-and-add for three
 * counters: the signal handlers in flight (app.c), which teardown has to
 * wait out and which no lock can guard, the GPU's frame-cost samples
 * (gpu_sample_seq, app.c), and the Debug trees' fault sites (fault.c). No
 * compare-and-swap. Every operation is sequentially
 * consistent.
 *
 * Not _Atomic, because MSVC supports C11 atomics only behind
 * /experimental:c11atomics.
 *
 * Structs rather than bare scalars, so that every access goes through the
 * functions below. A bare typedef would let `a->vsync = 0` compile as an
 * atomic store on one compiler and a plain one on the other.
 */

#if defined(_MSC_VER)
#  include <intrin.h>

typedef struct { volatile long v; }  FnlAtomicU32;
typedef struct { void *volatile v; } FnlAtomicPtr;

static inline uint32_t fnl_atomic_load_u32(const FnlAtomicU32 *a)
{
	/* An OR of zero is a read that participates in the lock protocol. MSVC has
	 * no plain seq-cst load intrinsic for this, and a volatile read alone is
	 * only ordered under /volatile:ms. */
	return (uint32_t)_InterlockedOr((volatile long *)&a->v, 0);
}
static inline void fnl_atomic_store_u32(FnlAtomicU32 *a, uint32_t v)
{
	_InterlockedExchange(&a->v, (long)v);
}
static inline void *fnl_atomic_load_ptr(const FnlAtomicPtr *a)
{
	return _InterlockedCompareExchangePointer((void *volatile *)&a->v, NULL,
											  NULL);
}
static inline void fnl_atomic_store_ptr(FnlAtomicPtr *a, void *v)
{
	_InterlockedExchangePointer(&a->v, v);
}
/* Returns the value before the add. Wraps, so adding UINT32_MAX subtracts. */
static inline uint32_t fnl_atomic_add_u32(FnlAtomicU32 *a, uint32_t d)
{
	return (uint32_t)_InterlockedExchangeAdd(&a->v, (long)d);
}

#else
#  include <stdatomic.h>

typedef struct { _Atomic uint32_t v; }  FnlAtomicU32;
typedef struct { _Atomic(void *) v; }   FnlAtomicPtr;

static inline uint32_t fnl_atomic_load_u32(const FnlAtomicU32 *a)
{
	return atomic_load(&a->v);
}
static inline void fnl_atomic_store_u32(FnlAtomicU32 *a, uint32_t v)
{
	atomic_store(&a->v, v);
}
static inline void *fnl_atomic_load_ptr(const FnlAtomicPtr *a)
{
	return atomic_load(&a->v);
}
static inline void fnl_atomic_store_ptr(FnlAtomicPtr *a, void *v)
{
	atomic_store(&a->v, v);
}
static inline uint32_t fnl_atomic_add_u32(FnlAtomicU32 *a, uint32_t d)
{
	return atomic_fetch_add(&a->v, d);
}
#endif

/* Booleans go through the 32-bit pair: MSVC's interlocked family starts at
 * 32 bits. The conversions below are where the truthiness is pinned. */
typedef FnlAtomicU32 FnlAtomicBool;

static inline bool fnl_atomic_load_bool(const FnlAtomicBool *a)
{
	return fnl_atomic_load_u32(a) != 0;
}
static inline void fnl_atomic_store_bool(FnlAtomicBool *a, bool v)
{
	fnl_atomic_store_u32(a, v ? 1u : 0u);
}

#endif /* FNL_COMPILER_H */
