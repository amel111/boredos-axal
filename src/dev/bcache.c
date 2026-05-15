// AXAL Block Cache — LRU sector cache implementation
// Dramatically reduces disk I/O for repeated reads (FAT table, directory entries, etc.)
//
// Copyright (c) 2023-2026 Chris (boreddevnl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.

#include "bcache.h"
#include "spinlock.h"
#include <stddef.h>

extern void mem_memcpy(void *dest, const void *src, size_t len);
extern void mem_memset(void *dest, int val, size_t len);
extern void serial_write(const char *str);
extern void serial_write_num(uint32_t n);

static bcache_entry_t cache[BCACHE_SIZE];
static spinlock_t bcache_lock = SPINLOCK_INIT;
static uint32_t tick_counter = 0;
static bool bcache_initialized = false;

// Statistics
static uint64_t stat_hits = 0;
static uint64_t stat_misses = 0;
static uint64_t stat_evictions = 0;
static uint64_t stat_writes = 0;

void bcache_init(void) {
    mem_memset(cache, 0, sizeof(cache));
    for (int i = 0; i < BCACHE_SIZE; i++) {
        cache[i].valid = false;
        cache[i].dirty = false;
        cache[i].disk = NULL;
        cache[i].sector = 0;
        cache[i].access_tick = 0;
    }
    bcache_initialized = true;
    serial_write("[BCACHE/AXAL] Block cache initialized: ");
    serial_write_num(BCACHE_SIZE);
    serial_write(" sectors (");
    serial_write_num(BCACHE_SIZE * SECTOR_SIZE / 1024);
    serial_write(" KB)\n");
}

// Find a cache entry matching disk+sector. Returns index or -1.
static int bcache_find(Disk *disk, uint32_t sector) {
    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].disk == disk && cache[i].sector == sector) {
            return i;
        }
    }
    return -1;
}

// Find the LRU (least recently used) entry to evict.
static int bcache_find_lru(void) {
    int lru_idx = 0;
    uint32_t lru_tick = UINT32_MAX;

    // First try to find an invalid (empty) slot
    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (!cache[i].valid) return i;
    }

    // All slots valid — find the least recently accessed
    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (cache[i].access_tick < lru_tick) {
            lru_tick = cache[i].access_tick;
            lru_idx = i;
        }
    }
    return lru_idx;
}

int bcache_read_sector(Disk *disk, uint32_t sector, uint8_t *buffer) {
    if (!bcache_initialized || !disk || !buffer) {
        // Fallback: direct read
        if (disk && disk->read_sector) return disk->read_sector(disk, sector, buffer);
        return -1;
    }

    uint64_t rflags = spinlock_acquire_irqsave(&bcache_lock);

    // Check cache
    int idx = bcache_find(disk, sector);
    if (idx >= 0) {
        // Cache hit
        cache[idx].access_tick = ++tick_counter;
        mem_memcpy(buffer, cache[idx].data, SECTOR_SIZE);
        stat_hits++;
        spinlock_release_irqrestore(&bcache_lock, rflags);
        return 0;
    }

    // Cache miss — need to read from disk
    stat_misses++;

    // Find slot to use (evict LRU if needed)
    idx = bcache_find_lru();

    // If evicting a dirty entry, flush it first
    if (cache[idx].valid && cache[idx].dirty) {
        if (cache[idx].disk && cache[idx].disk->write_sector) {
            // Release lock during disk I/O to avoid holding it too long
            Disk *evict_disk = cache[idx].disk;
            uint32_t evict_sector = cache[idx].sector;
            uint8_t evict_data[SECTOR_SIZE];
            mem_memcpy(evict_data, cache[idx].data, SECTOR_SIZE);
            spinlock_release_irqrestore(&bcache_lock, rflags);
            
            evict_disk->write_sector(evict_disk, evict_sector, evict_data);
            
            rflags = spinlock_acquire_irqsave(&bcache_lock);
            // Re-find our slot (it may have been reused while lock was released)
            idx = bcache_find_lru();
        }
        stat_evictions++;
    } else if (cache[idx].valid) {
        stat_evictions++;
    }

    // Read from disk (release lock during I/O)
    spinlock_release_irqrestore(&bcache_lock, rflags);
    
    uint8_t temp[SECTOR_SIZE];
    int result = -1;
    if (disk->read_sector) {
        result = disk->read_sector(disk, sector, temp);
    }

    if (result != 0) return result;

    // Store in cache
    rflags = spinlock_acquire_irqsave(&bcache_lock);
    
    // Re-check if someone else cached it while we were reading
    int existing = bcache_find(disk, sector);
    if (existing >= 0) {
        cache[existing].access_tick = ++tick_counter;
        mem_memcpy(buffer, cache[existing].data, SECTOR_SIZE);
        spinlock_release_irqrestore(&bcache_lock, rflags);
        return 0;
    }

    // Find slot again (may have changed)
    idx = bcache_find_lru();
    
    cache[idx].disk = disk;
    cache[idx].sector = sector;
    cache[idx].access_tick = ++tick_counter;
    cache[idx].valid = true;
    cache[idx].dirty = false;
    mem_memcpy(cache[idx].data, temp, SECTOR_SIZE);

    // Copy to caller's buffer
    mem_memcpy(buffer, temp, SECTOR_SIZE);

    spinlock_release_irqrestore(&bcache_lock, rflags);
    return 0;
}

int bcache_write_sector(Disk *disk, uint32_t sector, const uint8_t *buffer) {
    if (!bcache_initialized || !disk || !buffer) {
        if (disk && disk->write_sector) return disk->write_sector(disk, sector, buffer);
        return -1;
    }

    // Write-through: always write to disk
    int result = -1;
    if (disk->write_sector) {
        result = disk->write_sector(disk, sector, buffer);
    }
    if (result != 0) return result;

    stat_writes++;

    // Update cache
    uint64_t rflags = spinlock_acquire_irqsave(&bcache_lock);

    int idx = bcache_find(disk, sector);
    if (idx >= 0) {
        // Update existing cache entry
        mem_memcpy(cache[idx].data, buffer, SECTOR_SIZE);
        cache[idx].access_tick = ++tick_counter;
        cache[idx].dirty = false; // Already written to disk
    } else {
        // Insert into cache
        idx = bcache_find_lru();
        if (cache[idx].valid && cache[idx].dirty) {
            // Flush evicted dirty entry
            if (cache[idx].disk && cache[idx].disk->write_sector) {
                Disk *evict_disk = cache[idx].disk;
                uint32_t evict_sector = cache[idx].sector;
                uint8_t evict_data[SECTOR_SIZE];
                mem_memcpy(evict_data, cache[idx].data, SECTOR_SIZE);
                spinlock_release_irqrestore(&bcache_lock, rflags);
                evict_disk->write_sector(evict_disk, evict_sector, evict_data);
                rflags = spinlock_acquire_irqsave(&bcache_lock);
                idx = bcache_find_lru();
            }
        }
        cache[idx].disk = disk;
        cache[idx].sector = sector;
        cache[idx].access_tick = ++tick_counter;
        cache[idx].valid = true;
        cache[idx].dirty = false;
        mem_memcpy(cache[idx].data, buffer, SECTOR_SIZE);
    }

    spinlock_release_irqrestore(&bcache_lock, rflags);
    return 0;
}

void bcache_invalidate_disk(Disk *disk) {
    if (!bcache_initialized) return;

    uint64_t rflags = spinlock_acquire_irqsave(&bcache_lock);
    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].disk == disk) {
            cache[i].valid = false;
            cache[i].dirty = false;
        }
    }
    spinlock_release_irqrestore(&bcache_lock, rflags);
}

void bcache_flush(void) {
    if (!bcache_initialized) return;

    uint64_t rflags = spinlock_acquire_irqsave(&bcache_lock);
    for (int i = 0; i < BCACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].dirty && cache[i].disk) {
            if (cache[i].disk->write_sector) {
                // Release lock during I/O
                Disk *d = cache[i].disk;
                uint32_t s = cache[i].sector;
                uint8_t data[SECTOR_SIZE];
                mem_memcpy(data, cache[i].data, SECTOR_SIZE);
                spinlock_release_irqrestore(&bcache_lock, rflags);
                
                d->write_sector(d, s, data);
                
                rflags = spinlock_acquire_irqsave(&bcache_lock);
                cache[i].dirty = false;
            }
        }
    }
    spinlock_release_irqrestore(&bcache_lock, rflags);
}

bcache_stats_t bcache_get_stats(void) {
    return (bcache_stats_t){
        .hits = stat_hits,
        .misses = stat_misses,
        .evictions = stat_evictions,
        .writes = stat_writes,
    };
}
