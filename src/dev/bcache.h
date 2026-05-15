// AXAL Block Cache — LRU sector cache for disk I/O
// Caches recently read sectors to avoid redundant disk access.
// Write-through policy ensures data consistency.
//
// Copyright (c) 2023-2026 Chris (boreddevnl)
// This software is released under the GNU General Public License v3.0. See LICENSE file for details.
// This header needs to maintain in any file it is present in, as per the GPL license terms.
#ifndef BCACHE_H
#define BCACHE_H

#include <stdint.h>
#include <stdbool.h>
#include "disk.h"

// Number of cached sectors. 256 sectors = 128KB cache.
// Tunable: increase for better hit rates on sequential workloads.
#define BCACHE_SIZE 256

typedef struct bcache_entry {
    uint8_t  data[SECTOR_SIZE];
    Disk    *disk;              // Which disk this sector belongs to
    uint32_t sector;           // LBA sector number
    uint32_t access_tick;      // LRU timestamp
    bool     valid;            // Entry contains valid data
    bool     dirty;            // Entry has been written but not flushed
} bcache_entry_t;

// Initialize the block cache
void bcache_init(void);

// Read a sector through the cache. Returns 0 on success, -1 on error.
// On cache hit, copies from cache (no disk I/O). On miss, reads from disk and caches.
int bcache_read_sector(Disk *disk, uint32_t sector, uint8_t *buffer);

// Write a sector through the cache (write-through: writes to both cache and disk).
int bcache_write_sector(Disk *disk, uint32_t sector, const uint8_t *buffer);

// Invalidate all cache entries for a given disk (e.g., on disk removal)
void bcache_invalidate_disk(Disk *disk);

// Flush all dirty entries to disk
void bcache_flush(void);

// Get cache statistics
typedef struct {
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t writes;
} bcache_stats_t;

bcache_stats_t bcache_get_stats(void);

#endif // BCACHE_H
