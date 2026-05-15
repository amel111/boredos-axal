// AXAL Physical Memory Manager — Buddy Allocator
// Provides O(1) page allocation/free with O(log n) coalescing.
// Replaces the linear-scan approach for page-sized allocations.
//
// Copyright (c) 2023-2026 Chris (boreddevnl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "limine.h"

// Buddy allocator orders: 0 = 4KB, 1 = 8KB, ..., MAX_ORDER = 2MB
#define PMM_PAGE_SIZE    4096UL
#define PMM_MAX_ORDER    10          // 2^10 pages = 4MB max contiguous block
#define PMM_MAX_PAGES    (1024*1024) // Support up to 4GB of RAM (1M pages)

// Free list node — stored at the start of each free block
typedef struct pmm_free_block {
    struct pmm_free_block *next;
    struct pmm_free_block *prev;
} pmm_free_block_t;

// Per-order free list head
typedef struct {
    pmm_free_block_t *head;
    uint32_t count;
} pmm_free_list_t;

// Initialize the PMM from the Limine memory map
void pmm_init(struct limine_memmap_response *memmap);

// Allocate 2^order contiguous physical pages. Returns physical address or 0 on failure.
uint64_t pmm_alloc_pages(uint32_t order);

// Allocate a single physical page (order 0). Returns physical address or 0.
static inline uint64_t pmm_alloc_page(void) {
    return pmm_alloc_pages(0);
}

// Free 2^order contiguous physical pages starting at the given physical address.
void pmm_free_pages(uint64_t phys_addr, uint32_t order);

// Free a single physical page.
static inline void pmm_free_page(uint64_t phys_addr) {
    pmm_free_pages(phys_addr, 0);
}

// Get total free pages across all orders
uint64_t pmm_free_page_count(void);

// Get total managed pages
uint64_t pmm_total_page_count(void);

#endif // PMM_H
