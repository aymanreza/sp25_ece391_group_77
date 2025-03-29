#include <stdlib.h>
#include <string.h>
#include "cache.h"
#include "ktfs.h"

#define CACHE_CAPACITY 64

typedef struct cache_block {
    unsigned long long pos;
    void *data;
    int dirty;
    int refcount;
    //add mroe stuff
} cache_block_t;

struct cache {
    struct io *bkgio;
    cache_block_t blocks[CACHE_CAPACITY];
//add more stuff 
};
int create_cache(struct io *bkgio,struct cache ** cptr )
{

}
int cache_get_block(struct cache *cache, unsigned long long pos, void ** pptr )
{

}

void cache_release_block(struct cache *cache,void *pblk,int dirty)
{
    for (int i = 0; i < CACHE_CAPACITY; i++) {
        cache_block_t *block = &cache->blocks[i];
        if(block->data==pblk)
        {
            if(dirty==1)
            {
                block->dirty=1;
            }
            if(dirty==0)
            {
                block->dirty=0;
            }
            if(block->refcount>0)
            {
                block->refcount--;
            }
            return;
        }
    }
    
}
int cache_flush(struct cache * 	cache)
{
    for(int i=0; i<CACHE_CAPACITY; i++)
    {
        cache_block_t *block = &cache->blocks[i];
            int backing_device = iowriteat(cache->bkgio, block->pos, block->data, CACHE_BLKSZ);
            if (backing_device != CACHE_BLKSZ) 
                {
                    return -1;
                }

                block->dirty = 0;
    } 	
    return 0;
}