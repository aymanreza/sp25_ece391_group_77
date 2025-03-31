// cache.c - CACHE implementation
//
#include "heap.h"
#include "fs.h"
#include "ioimpl.h"
#include "ktfs.h"
#include "error.h"
#include "thread.h"
#include "string.h"
#include "console.h"
#include "cache.h"



struct cache_entry {
    uint64_t blocknum; // block bumber from disk cache pulled from
    char data[CACHE_BLKSZ]; // actual data from the block
    int valid; // if entry contains data and in use
    int dirty; // whether we wrote to this block or not
};

struct cache {
    struct io *bdev;
    struct cache_entry entries[CACHE_CAPACITY]; // capacity is 64
};


int create_cache(struct io * bkgio, struct cache ** cptr) {
    // Check if inputs are valid
    if (!bkgio || !cptr) return -EINVAL;

    // Allocate the cache structure
    struct cache *cache = kcalloc(1, sizeof(struct cache));
    if (!cache) return -ENOMEM;

    cache->bdev = ioaddref(bkgio);  // Store backing device and increment ref count

    // Initialize cache entries
    for (int i = 0; i < CACHE_CAPACITY; i++) {
        cache->entries[i].valid = CACHE_INVALID;
        cache->entries[i].dirty = CACHE_CLEAN;
        cache->entries[i].blocknum = 0;  
    }

    // Return it to the caller
    *cptr = cache;
    return 0;
}

int cache_get_block(struct cache * cache, unsigned long long pos, void ** pptr) {
    // checking validity of arguments
    if (!cache || !pptr || pos % CACHE_BLKSZ != 0)
        return -EINVAL;

    uint64_t blocknum = pos / CACHE_BLKSZ; // getting block number (pos is multiple of 512)

    // look for block cache
    for (int i = 0; i < CACHE_CAPACITY; i++) { // going through each cache entry
        struct cache_entry *entry = &cache->entries[i];
        if (entry->valid && entry->blocknum == blocknum) { // found block number
            *pptr = entry->data; // return pointer to the correct block
            return 0;
        }
    }

    // block is not in cache, picking a victim to evict 
    int victim = -1;
    for (int i = 0; i < CACHE_CAPACITY; i++) {
        if (!cache->entries[i].valid) { // looking for blocks with no data
            victim = i; // victim is chosen
            break;
        }
    }

    if (victim == -1) // if victim is never chosen, 
        victim = 0; // we just choose entry #0

    struct cache_entry *entry = &cache->entries[victim];

    // if dirty, flush to disk
    if (entry->valid && entry->dirty) {
        int ret = iowriteat(cache->bdev, entry->blocknum * CACHE_BLKSZ, entry->data, CACHE_BLKSZ);
        if (ret < 0) return ret; // fail
    }

    // read new block from disk
    int ret = ioreadat(cache->bdev, pos, entry->data, CACHE_BLKSZ);
    if (ret != CACHE_BLKSZ)
        return -EIO;

    // update cache entry
    entry->valid = CACHE_VALID;
    entry->dirty = CACHE_CLEAN;
    entry->blocknum = blocknum;

    *pptr = entry->data; // return pointer to the correct data block
    return 0;
}

void cache_release_block(struct cache * cache, void * pblk, int dirty) {
    // validating arguments
    if (!cache || !pblk) return;

    // going through each entry
    for (int i = 0; i < CACHE_CAPACITY; i++) {
        struct cache_entry *entry = &cache->entries[i];

        // if there is data in the block, and it matches the blk returned from get block...
        if (entry->valid && entry->data == pblk) {
            if (dirty) // mark dirty if written to
                entry->dirty = CACHE_DIRTY;
            return;
        }
    }
}

int cache_flush(struct cache * cache) {
    // validating arguments
    if (!cache || !cache->bdev) return -EINVAL;

    // going through each entry 
    for (int i = 0; i < CACHE_CAPACITY; i++) {
        struct cache_entry *entry = &cache->entries[i];

        if (entry->valid && entry->dirty) { // writing cache blocks with data back into backing device
            int ret = iowriteat(cache->bdev,
                                entry->blocknum * CACHE_BLKSZ,
                                entry->data,
                                CACHE_BLKSZ);

            if (ret != CACHE_BLKSZ) // sanity check if 512 bytes not written
                return -EIO;

            entry->dirty = 0;  // clear dirty bit
        }
    }

    return 0;
}