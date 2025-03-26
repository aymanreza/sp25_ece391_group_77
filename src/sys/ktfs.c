// ktfs.c - KTFS implementation
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif


#include "heap.h"
#include "fs.h"
#include "ioimpl.h"
#include "ktfs.h"
#include "error.h"
#include "thread.h"
#include "string.h"
#include "console.h"
#include "cache.h"

// INTERNAL TYPE DEFINITIONS
//

struct ktfs_file {
    // Fill to fulfill spec
    struct io io;              // unified I/O interface
    unsigned int size;
    unsigned int inode_num;
    int flags; // for in use
    int ioref;
};

// global file system
struct ktfs {
    struct io *bdev;               // underlying block device
    struct ktfs_superblock sb;     // loaded from block 0
    
    // maybe add cache
} fs;

// INTERNAL FUNCTION DECLARATIONS
//

int ktfs_mount(struct io * io);

int ktfs_open(const char * name, struct io ** ioptr);
void ktfs_close(struct io* io);
long ktfs_readat(struct io* io, unsigned long long pos, void * buf, long len);
int ktfs_cntl(struct io *io, int cmd, void *arg);

int ktfs_getblksz(struct ktfs_file *fd);
int ktfs_getend(struct ktfs_file *fd, void *arg);

int ktfs_flush(void);

// FUNCTION ALIASES
//

int fsmount(struct io * io)
    __attribute__ ((alias("ktfs_mount")));

int fsopen(const char * name, struct io ** ioptr)
    __attribute__ ((alias("ktfs_open")));

int fsflush(void)
    __attribute__ ((alias("ktfs_flush")));

// EXPORTED FUNCTION DEFINITIONS
//

int ktfs_mount(struct io * io) {
    // checking validity of io argument
    if (!io) 
        return -EINVAL;

    fs.bdev = ioaddref(io); // adding io reference (backing device) and storing into filesystem struct

    static char buf[KTFS_BLKSZ]; // buffer to read superblock

    int ret = ioreadat(fs.bdev, 0, buf, KTFS_BLKSZ); // reading data from superblock (first block)

    if(ret != KTFS_BLKSZ) //sanity check, making sure read size is 512
        return -EIO;

    memcpy(&fs.sb, buf, sizeof(struct ktfs_superblock)); //copying the data retrieved into driver memory

    // sanity check to make sure the super block is not unitilized
    if (fs.sb.block_count == 0 ||
        fs.sb.bitmap_block_count == 0 ||
        fs.sb.inode_block_count == 0 ||
        fs.sb.root_directory_inode == 0) {
        return -EINVAL;
    }

    // return success
    return 0;
}

int ktfs_read_inode(uint16_t num, struct ktfs_inode *out)

int ktfs_open(const char * name, struct io ** ioptr) {
    if(!name || !ioptr)
        return -EINVAL; // error invalid

    // read from inode using superblock root_directory_inode


    struct ktfs_dir_entry dir;

    for (int inode_idx = 0; inode_idx < FILEMAX; inode_idx++) { 
        if(strncmp(name, dir.name[i], KTFS_MAX_FILENAME_LEN) == 0){
            // saving the file on driver
            (struct ktfs_file *)file = kmalloc(sizeof(struct ktfs_file));

            file->io =               
            file->size =
            file->node_num = inode_idx;
            file->flags = KTFS_FILE_IN_USE;
        }
    }
 
}
void ktfs_close(struct io* io)
{
    if (!io) 
        return; 

    struct ktfs_file *file = (struct ktfs_file *)io;
    file->flags_in_use=0; 
    //global array curently open files 

}

long ktfs_readat(struct io* io, unsigned long long pos, void * buf, long len)
{
    return 0;
}

int ktfs_cntl(struct io *io, int cmd, void *arg)
{
    return 0;
}

int ktfs_flush(void)
{
    struct cache *global_cache; 
    if (global_cache==0) 
    {
        return 0; 
    }
    return cache_flush(global_cache); 
    
}