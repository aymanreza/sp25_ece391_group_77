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
    struct cache_entry *next; // link to next entry
};

struct cache {
    struct io *bdev;
    struct cache_entry *head; // head of the linked list
    struct cache_entry *head; // head of the linked list
    struct lock cache_lock;
    int size; // size of cache, number of entries
};

// Inputs:  struct io * bkgio- Backing I/O device used for reading and writing block
//struct cache **cptr - pointer to where create structure will be stored
// Outputs:  int - Returns 0 on success, or a negative failure  
// Description: it allocate annd intialize the new cache structure. It set all entries all invalid and
//assicciate the backing device with the cache and return to the pointer  
// Side Effects: Allocate memory for the cache and increments the refrence count of the device
    int size; // size of cache, number of entries
};


int create_cache(struct io * bkgio, struct cache ** cptr) {
    // check if inputs are valid
    // check if inputs are valid
    if (!bkgio || !cptr) return -EINVAL;

    // allocate the cache structure
    // allocate the cache structure
    struct cache *cache = kcalloc(1, sizeof(struct cache));
    if (!cache) return -ENOMEM;

    // initialize cache entries
    // initialize cache entries
    lock_init(&cache->cache_lock); //initililizing the lvok
    cache->bdev = ioaddref(bkgio);  // Store backing device and increment ref count
    cache->head = NULL;
    cache->size = 0;
    cache->head = NULL;
    cache->size = 0;

    // return it to the caller
    // return it to the caller
    *cptr = cache;
    return 0;
}
// Inputs:  struct cache *cache - it will pointy to the chche struture
// unsigned long long pos - bytes offset to the device
//void **pptr - the output pointer to receive the adresss of the cache block data  
// Outputs: int - Returns 0 on success, or a negative failure
// Description: it will retrive the block of the data at a spercific postion. If the block
//is alreafy cache, it will return the pointer. However, it will selects the victim block to
//write back to the disk if it dirty, load the requersted block from bakcing device and update the cache
// Side Effects: It will wirte the dirty cache black to the disk andf may read a block form the disk into cache.
//it also modifies the internal cache state.

int cache_get_block(struct cache * cache, unsigned long long pos, void ** pptr) {
    //verifying arguments
    //verifying arguments
    if (!cache || !pptr || pos % CACHE_BLKSZ != 0)
        return -EINVAL;

    //acquiring lock
    lock_acquire(&cache->cache_lock);
    uint64_t blocknum = pos / CACHE_BLKSZ; //caclulating block number
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

// Inputs: struct cache *cache - it will point to the cache structure
// void *pblk - it will pointer to the data block previously return to the release block
// int dirty - it will indicate whatever the block was modifed
// Outputs: None
// Description: release the blcok previously retrive by the get block. If the block is found in the cache
//and the dirty is set, the block is marked diry so it can be flished to the disk.
// Side Effects: It mark the block in the cache as dirty if it modifed and release the cache lock.


void cache_release_block(struct cache * cache, void * pblk, int dirty) {
    //validating arguemtns
    //validating arguemtns
    if (!cache || !pblk) return;

    lock_acquire(&cache->cache_lock); //aqcuring locks
    struct cache_entry *entry = cache->head;
    while (entry) { // while entry exists, check if it is valid and has data
        if (entry->valid && entry->data == pblk) { //see if it mataches block
            if (dirty) //check if its dirtty

    lock_acquire(&cache->cache_lock); //aqcuring locks
    struct cache_entry *entry = cache->head;
    while (entry) { // while entry exists, check if it is valid and has data
        if (entry->valid && entry->data == pblk) { //see if it mataches block
            if (dirty) //check if its dirtty
                entry->dirty = CACHE_DIRTY;
            break;
            break;
        }
        entry = entry->next; //update list
        entry = entry->next; //update list
    }
    lock_release(&cache->cache_lock); //release lock
    lock_release(&cache->cache_lock); //release lock
}


// Inputs:  struct cache *cache - it will point to the cache structure to be flushed
// Outputs: int - Returns 0 on success, or a negative failure
// Description: It will iterates through all the vaild entries in the cache and write back to any dirty block
// to the backing device. After a sucessful wirte, it clear the dirty flag.
// Side Effects: It will write operation to the backing device and modifies to the cache states by clearing dirty bits.
//It would also acquires and release the cache lock.

int cache_flush(struct cache * cache) {
    //validating arguments
    //validating arguments
    if (!cache || !cache->bdev) return -EINVAL;

    //getting lock
    lock_acquire(&cache->cache_lock);
    struct cache_entry *entry = cache->head;
    while (entry) { //while there is an entry
        if (entry->valid && entry->dirty) { //if its valid and somemthing is actually written to the block
            int ret = iowriteat(cache->bdev, entry->blocknum * CACHE_BLKSZ, entry->data, CACHE_BLKSZ); //perform write to device
            if (ret != CACHE_BLKSZ) { //validation

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
            entry->dirty = CACHE_CLEAN; //update to mark clean
        }
        entry = entry->next; //update list
        entry = entry->next; //update list
    }
    lock_release(&cache->cache_lock); //release lock
    lock_release(&cache->cache_lock); //release lock
    return 0;
}


