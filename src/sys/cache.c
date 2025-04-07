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
    struct cache_entry *next; // link to next entry
};

struct cache {
    struct io *bdev;
    struct cache_entry *head; // head of the linked list
    struct lock cache_lock;
    int size; // size of cache, number of entries
};


int create_cache(struct io * bkgio, struct cache ** cptr) {
    // check if inputs are valid
    if (!bkgio || !cptr) return -EINVAL;

    // allocate the cache structure
    struct cache *cache = kcalloc(1, sizeof(struct cache));
    if (!cache) return -ENOMEM;

    // initialize cache entries
    lock_init(&cache->cache_lock); //initililizing the lvok
    cache->bdev = ioaddref(bkgio);  // Store backing device and increment ref count
    cache->head = NULL;
    cache->size = 0;

    // return it to the caller
    *cptr = cache;
    return 0;
}

int cache_get_block(struct cache * cache, unsigned long long pos, void ** pptr) {
    //verifying arguments
    if (!cache || !pptr || pos % CACHE_BLKSZ != 0)
        return -EINVAL;

    //acquiring lock
    lock_acquire(&cache->cache_lock);
    uint64_t blocknum = pos / CACHE_BLKSZ; //caclulating block number

    // Search for cache hit through linked list
    // struct cache_entry *prev = NULL;
    struct cache_entry *curr = cache->head;
    while (curr) {
        if (curr->valid && curr->blocknum == blocknum) {
            *pptr = curr->data;
            lock_release(&cache->cache_lock);
            return 0;
        }
        // prev = curr;
        curr = curr->next;
    }

    // if full, evict least-recently-used (head of linked list)
    if (cache->size >= CACHE_CAPACITY) { //if size is greater than 64 entries,
        struct cache_entry *victim = cache->head; //removing head
        cache->head = victim->next; 
        cache->size--;

        if (victim->valid && victim->dirty) {
            int ret = iowriteat(cache->bdev, victim->blocknum * CACHE_BLKSZ, victim->data, CACHE_BLKSZ); //write at the block
            if (ret < 0) { //if fail,
                lock_release(&cache->cache_lock);
                return ret; //return
            }
        }

        kfree(victim); //free victim memory on heap
    }

    // sllocate new entry
    struct cache_entry *new_entry = kcalloc(1, sizeof(struct cache_entry));
    if (!new_entry) { //validation
        lock_release(&cache->cache_lock);
        return -ENOMEM;
    }

    int ret = ioreadat(cache->bdev, pos, new_entry->data, CACHE_BLKSZ);
    if (ret != CACHE_BLKSZ) { //validating
        kfree(new_entry);
        lock_release(&cache->cache_lock);
        return -EIO;
    }

    new_entry->valid = CACHE_VALID; // initializing new entry
    new_entry->dirty = CACHE_CLEAN;
    new_entry->blocknum = blocknum;
    new_entry->next = NULL;

    // append to list (inserting at tail)
    if (!cache->head) {
        cache->head = new_entry;
    } else {
        struct cache_entry *iter = cache->head; // head
        while (iter->next) iter = iter->next;
        iter->next = new_entry;
    }

    cache->size++;//increasing size
    *pptr = new_entry->data;
    lock_release(&cache->cache_lock);
    return 0;
}


void cache_release_block(struct cache * cache, void * pblk, int dirty) {
    //validating arguemtns
    if (!cache || !pblk) return;

    lock_acquire(&cache->cache_lock); //aqcuring locks
    struct cache_entry *entry = cache->head;
    while (entry) { // while entry exists, check if it is valid and has data
        if (entry->valid && entry->data == pblk) { //see if it mataches block
            if (dirty) //check if its dirtty
                entry->dirty = CACHE_DIRTY;
            break;
        }
        entry = entry->next; //update list
    }
    lock_release(&cache->cache_lock); //release lock
}


int cache_flush(struct cache * cache) {
    //validating arguments
    if (!cache || !cache->bdev) return -EINVAL;

    //getting lock
    lock_acquire(&cache->cache_lock);
    struct cache_entry *entry = cache->head;
    while (entry) { //while there is an entry
        if (entry->valid && entry->dirty) { //if its valid and somemthing is actually written to the block
            int ret = iowriteat(cache->bdev, entry->blocknum * CACHE_BLKSZ, entry->data, CACHE_BLKSZ); //perform write to device
            if (ret != CACHE_BLKSZ) { //validation
                lock_release(&cache->cache_lock);
                return -EIO;
            }
            entry->dirty = CACHE_CLEAN; //update to mark clean
        }
        entry = entry->next; //update list
    }
    lock_release(&cache->cache_lock); //release lock
    return 0;
}