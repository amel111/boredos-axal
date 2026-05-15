// AXAL Per-CPU Slab Magazine Cache
// Eliminates global lock contention for small allocations by maintaining
// per-CPU freelists. Each CPU has a "magazine" of pre-allocated objects.
// When a magazine empties, it refills from the global slab in batch.
//
// Copyright (c) 2023-2026 Chris (boreddevnl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef PCPU_CACHE_H
#define PCPU_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Magazine size — number of objects cached per CPU per size class
#define MAGAZINE_SIZE 32
#define PCPU_MAX_CPUS 32
#define PCPU_SIZE_CLASSES 7  // 8, 16, 32, 64, 128, 256, 512

// Per-CPU magazine for a single size class
typedef struct {
    void *objects[MAGAZINE_SIZE];
    uint32_t count;  // Number of objects currently in magazine
} __attribute__((aligned(64))) pcpu_magazine_t;  // Cache-line aligned

// Initialize per-CPU caches (call after SMP init)
void pcpu_cache_init(void);

// Allocate from per-CPU cache. Falls back to global allocator if magazine empty.
// Returns NULL if both per-CPU and global are exhausted.
void *pcpu_alloc(size_t size);

// Free to per-CPU cache. Overflows to global allocator if magazine full.
void pcpu_free(void *ptr, size_t size);

#endif // PCPU_CACHE_H
