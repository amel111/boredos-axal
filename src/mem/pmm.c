// AXAL Physical Memory Manager — Buddy Allocator Implementation
// O(1) allocation for common case, O(log n) split/coalesce.
// Per-page bitmap tracks allocation state for buddy coalescing.
//
// Copyright (c) 2023-2026 Chris (boreddevnl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "pmm.h"
#include "platform.h"
#include "spinlock.h"
#include "limine.h"
#include <stdint.h>
#include <stddef.h>

extern void serial_write(const char *str);
extern void serial_write_num(uint32_t n);
extern void mem_memset(void *dest, int val, size_t len);

// Free lists for each order (0..MAX_ORDER)
static pmm_free_list_t free_lists[PMM_MAX_ORDER + 1];

// Bitmap: 1 bit per page. Bit set = allocated or unavailable.
// We use a static array sized for up to 4GB. For larger RAM, this would need dynamic sizing.
static uint8_t page_bitmap[PMM_MAX_PAGES / 8];

static uint64_t pmm_base_addr = 0;       // Lowest physical address we manage
static uint64_t pmm_total_pages = 0;     // Total pages under management
static uint64_t pmm_free_pages_count = 0;
static bool pmm_initialized = false;
static spinlock_t pmm_lock = SPINLOCK_INIT;

// --- Bitmap helpers ---

static inline void bitmap_set(uint64_t page_idx) {
    page_bitmap[page_idx / 8] |= (1 << (page_idx % 8));
}

static inline void bitmap_clear(uint64_t page_idx) {
    page_bitmap[page_idx / 8] &= ~(1 << (page_idx % 8));
}

static inline bool bitmap_test(uint64_t page_idx) {
    return (page_bitmap[page_idx / 8] >> (page_idx % 8)) & 1;
}

// --- Free list helpers ---

static void list_insert(pmm_free_list_t *list, pmm_free_block_t *block) {
    block->prev = NULL;
    block->next = list->head;
    if (list->head) list->head->prev = block;
    list->head = block;
    list->count++;
}

static void list_remove(pmm_free_list_t *list, pmm_free_block_t *block) {
    if (block->prev) block->prev->next = block->next;
    else list->head = block->next;
    if (block->next) block->next->prev = block->prev;
    block->next = NULL;
    block->prev = NULL;
    list->count--;
}

// --- Core buddy operations ---

// Convert physical address to page index relative to pmm_base_addr
static inline uint64_t phys_to_page_idx(uint64_t phys) {
    return (phys - pmm_base_addr) / PMM_PAGE_SIZE;
}

// Convert page index to physical address
static inline uint64_t page_idx_to_phys(uint64_t idx) {
    return pmm_base_addr + idx * PMM_PAGE_SIZE;
}

// Get the buddy's page index for a block at page_idx of given order
static inline uint64_t buddy_idx(uint64_t page_idx, uint32_t order) {
    return page_idx ^ (1UL << order);
}

// Check if all pages in a block are free (not set in bitmap)
static bool block_is_free(uint64_t page_idx, uint32_t order) {
    uint64_t count = 1UL << order;
    for (uint64_t i = 0; i < count; i++) {
        if (bitmap_test(page_idx + i)) return false;
    }
    return true;
}

// Mark all pages in a block as allocated
static void block_mark_allocated(uint64_t page_idx, uint32_t order) {
    uint64_t count = 1UL << order;
    for (uint64_t i = 0; i < count; i++) {
        bitmap_set(page_idx + i);
    }
}

// Mark all pages in a block as free
static void block_mark_free(uint64_t page_idx, uint32_t order) {
    uint64_t count = 1UL << order;
    for (uint64_t i = 0; i < count; i++) {
        bitmap_clear(page_idx + i);
    }
}

void pmm_init(struct limine_memmap_response *memmap) {
    if (pmm_initialized || !memmap) return;

    // Clear all state
    mem_memset(free_lists, 0, sizeof(free_lists));
    mem_memset(page_bitmap, 0xFF, sizeof(page_bitmap)); // All pages start as "allocated/unavailable"

    // Find the lowest usable physical address
    uint64_t lowest = UINT64_MAX;
    uint64_t highest = 0;

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        if (e->base < 0x100000) continue; // Skip first 1MB
        if (e->base < lowest) lowest = e->base;
        uint64_t end = e->base + e->length;
        if (end > highest) highest = end;
    }

    if (lowest == UINT64_MAX) return;

    // Align base down to page boundary
    pmm_base_addr = lowest & ~(PMM_PAGE_SIZE - 1);
    pmm_total_pages = (highest - pmm_base_addr) / PMM_PAGE_SIZE;
    if (pmm_total_pages > PMM_MAX_PAGES) pmm_total_pages = PMM_MAX_PAGES;

    // Now add usable regions to the buddy allocator
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t base = e->base;
        uint64_t size = e->length;

        // Skip first 1MB
        if (base < 0x100000) {
            if (base + size <= 0x100000) continue;
            size -= (0x100000 - base);
            base = 0x100000;
        }

        // Align base up to page boundary
        uint64_t aligned_base = (base + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
        if (aligned_base >= base + size) continue;
        size -= (aligned_base - base);
        base = aligned_base;

        // Align size down to page boundary
        size &= ~(PMM_PAGE_SIZE - 1);
        if (size < PMM_PAGE_SIZE) continue;

        // Add pages to free lists using largest possible buddy blocks
        uint64_t addr = base;
        uint64_t end = base + size;

        while (addr < end) {
            uint64_t page_idx = phys_to_page_idx(addr);
            
            // Find the largest order block that:
            // 1. Fits within remaining space
            // 2. Is naturally aligned (page_idx is aligned to 2^order)
            uint32_t order = 0;
            while (order < PMM_MAX_ORDER) {
                uint64_t block_size = (1UL << (order + 1)) * PMM_PAGE_SIZE;
                uint64_t alignment = 1UL << (order + 1);
                if (addr + block_size > end) break;
                if (page_idx & (alignment - 1)) break;
                order++;
            }

            // Mark pages as free in bitmap
            block_mark_free(page_idx, order);

            // Insert into free list
            pmm_free_block_t *block = (pmm_free_block_t *)p2v(addr);
            list_insert(&free_lists[order], block);
            pmm_free_pages_count += (1UL << order);

            addr += (1UL << order) * PMM_PAGE_SIZE;
        }
    }

    pmm_initialized = true;

    serial_write("[PMM/AXAL] Buddy allocator initialized: ");
    serial_write_num((uint32_t)(pmm_free_pages_count * PMM_PAGE_SIZE / 1024 / 1024));
    serial_write(" MB free across ");
    serial_write_num((uint32_t)pmm_free_pages_count);
    serial_write(" pages\n");
}

uint64_t pmm_alloc_pages(uint32_t order) {
    if (!pmm_initialized || order > PMM_MAX_ORDER) return 0;

    uint64_t rflags = spinlock_acquire_irqsave(&pmm_lock);

    // Find the smallest order with a free block >= requested order
    uint32_t current_order = order;
    while (current_order <= PMM_MAX_ORDER && !free_lists[current_order].head) {
        current_order++;
    }

    if (current_order > PMM_MAX_ORDER) {
        // No memory available
        spinlock_release_irqrestore(&pmm_lock, rflags);
        return 0;
    }

    // Remove block from free list
    pmm_free_block_t *block = free_lists[current_order].head;
    list_remove(&free_lists[current_order], block);

    uint64_t phys = v2p((uint64_t)block);
    uint64_t page_idx = phys_to_page_idx(phys);

    // Split larger blocks down to requested order
    while (current_order > order) {
        current_order--;
        // The upper half becomes a free buddy
        uint64_t buddy_page = page_idx + (1UL << current_order);
        uint64_t buddy_phys = page_idx_to_phys(buddy_page);
        pmm_free_block_t *buddy_block = (pmm_free_block_t *)p2v(buddy_phys);
        list_insert(&free_lists[current_order], buddy_block);
    }

    // Mark allocated in bitmap
    block_mark_allocated(page_idx, order);
    pmm_free_pages_count -= (1UL << order);

    spinlock_release_irqrestore(&pmm_lock, rflags);
    return phys;
}

void pmm_free_pages(uint64_t phys_addr, uint32_t order) {
    if (!pmm_initialized || order > PMM_MAX_ORDER) return;
    if (phys_addr < pmm_base_addr) return;

    uint64_t page_idx = phys_to_page_idx(phys_addr);
    if (page_idx >= pmm_total_pages) return;

    uint64_t rflags = spinlock_acquire_irqsave(&pmm_lock);

    // Mark free in bitmap
    block_mark_free(page_idx, order);
    pmm_free_pages_count += (1UL << order);

    // Coalesce with buddy if possible
    uint32_t current_order = order;
    uint64_t current_idx = page_idx;

    while (current_order < PMM_MAX_ORDER) {
        uint64_t buddy = buddy_idx(current_idx, current_order);
        
        // Check bounds
        if (buddy >= pmm_total_pages) break;

        // Check if buddy is completely free
        if (!block_is_free(buddy, current_order)) break;

        // Check alignment — the merged block must be aligned to the next order
        uint64_t merged_idx = (current_idx < buddy) ? current_idx : buddy;
        if (merged_idx & ((1UL << (current_order + 1)) - 1)) break;

        // Remove buddy from its free list
        uint64_t buddy_phys = page_idx_to_phys(buddy);
        pmm_free_block_t *buddy_block = (pmm_free_block_t *)p2v(buddy_phys);
        
        // Verify buddy is actually in the free list (linear scan — only needed for correctness)
        pmm_free_block_t *scan = free_lists[current_order].head;
        bool found = false;
        while (scan) {
            if (scan == buddy_block) { found = true; break; }
            scan = scan->next;
        }
        if (!found) break;

        list_remove(&free_lists[current_order], buddy_block);

        // Merge: use the lower address as the new block
        current_idx = merged_idx;
        current_order++;
    }

    // Insert merged block into appropriate free list
    uint64_t block_phys = page_idx_to_phys(current_idx);
    pmm_free_block_t *block = (pmm_free_block_t *)p2v(block_phys);
    list_insert(&free_lists[current_order], block);

    spinlock_release_irqrestore(&pmm_lock, rflags);
}

uint64_t pmm_free_page_count(void) {
    return pmm_free_pages_count;
}

uint64_t pmm_total_page_count(void) {
    return pmm_total_pages;
}
