// AXAL Per-CPU Slab Magazine Cache Implementation
// Lock-free fast path: each CPU allocates/frees from its own magazine without locks.
// Slow path: refill/drain magazine from/to the global slab allocator (takes global lock).
//
// Copyright (c) 2023-2026 Chris (boreddevnl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "pcpu_cache.h"
#include "memory_manager.h"
#include "spinlock.h"
#include "platform.h"

extern void serial_write(const char *str);
extern uint32_t smp_this_cpu_id(void);

static const uint16_t pcpu_sizes[PCPU_SIZE_CLASSES] = {8, 16, 32, 64, 128, 256, 512};

// Per-CPU magazines: [cpu_id][size_class]
static pcpu_magazine_t magazines[PCPU_MAX_CPUS][PCPU_SIZE_CLASSES];
static bool pcpu_initialized = false;

// Batch refill count — how many objects to grab from global allocator at once
#define REFILL_BATCH 16

static int size_to_class(size_t size) {
    for (int i = 0; i < PCPU_SIZE_CLASSES; i++) {
        if (size <= pcpu_sizes[i]) return i;
    }
    return -1;
}

void pcpu_cache_init(void) {
    // Zero all magazines
    for (int cpu = 0; cpu < PCPU_MAX_CPUS; cpu++) {
        for (int cls = 0; cls < PCPU_SIZE_CLASSES; cls++) {
            magazines[cpu][cls].count = 0;
            for (int i = 0; i < MAGAZINE_SIZE; i++) {
                magazines[cpu][cls].objects[i] = NULL;
            }
        }
    }
    pcpu_initialized = true;
    serial_write("[PCPU/AXAL] Per-CPU magazine caches initialized\n");
}

void *pcpu_alloc(size_t size) {
    if (!pcpu_initialized) return kmalloc(size);

    int cls = size_to_class(size);
    if (cls < 0) return kmalloc(size); // Too large for magazine

    uint32_t cpu = smp_this_cpu_id();
    if (cpu >= PCPU_MAX_CPUS) return kmalloc(size);

    pcpu_magazine_t *mag = &magazines[cpu][cls];

    // Fast path: pop from local magazine (no lock needed — per-CPU)
    if (mag->count > 0) {
        mag->count--;
        void *obj = mag->objects[mag->count];
        mag->objects[mag->count] = NULL;
        return obj;
    }

    // Slow path: refill magazine from global allocator
    // We batch-allocate to amortize the lock acquisition cost
    uint32_t refilled = 0;
    for (uint32_t i = 0; i < REFILL_BATCH; i++) {
        void *obj = kmalloc(pcpu_sizes[cls]);
        if (!obj) break;
        mag->objects[mag->count] = obj;
        mag->count++;
        refilled++;
    }

    if (refilled == 0) return NULL;

    // Return one object from the freshly refilled magazine
    mag->count--;
    void *result = mag->objects[mag->count];
    mag->objects[mag->count] = NULL;
    return result;
}

void pcpu_free(void *ptr, size_t size) {
    if (!pcpu_initialized || !ptr) {
        kfree(ptr);
        return;
    }

    int cls = size_to_class(size);
    if (cls < 0) {
        kfree(ptr);
        return;
    }

    uint32_t cpu = smp_this_cpu_id();
    if (cpu >= PCPU_MAX_CPUS) {
        kfree(ptr);
        return;
    }

    pcpu_magazine_t *mag = &magazines[cpu][cls];

    // Fast path: push to local magazine (no lock needed)
    if (mag->count < MAGAZINE_SIZE) {
        mag->objects[mag->count] = ptr;
        mag->count++;
        return;
    }

    // Slow path: magazine full — drain half back to global allocator
    uint32_t drain_count = MAGAZINE_SIZE / 2;
    for (uint32_t i = 0; i < drain_count; i++) {
        mag->count--;
        kfree(mag->objects[mag->count]);
        mag->objects[mag->count] = NULL;
    }

    // Now there's room — store the freed object
    mag->objects[mag->count] = ptr;
    mag->count++;
}
