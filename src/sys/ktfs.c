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
    int flags;
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


// HELPER FUNCTIONS
//

int ktfs_read_inode(uint16_t inum, struct ktfs_inode *out) {
    // checking validity of the arguments
    if (!out || inum == 0) return -EINVAL;

    //calculating the indexes to retricing the correct inode block
    const int inodes_per_block = KTFS_BLKSZ / KTFS_INOSZ; // our case we have 16 inodes per block
    uint32_t block_idx = 1 + fs.sb.bitmap_block_count + (inum / inodes_per_block); // which inode block we are in
    uint32_t offset = (inum % inodes_per_block) * KTFS_INOSZ; // which inode INSIDE the block we want

    // buffer to fill using read
    char buf[KTFS_BLKSZ];
    int ret = ioreadat(fs.bdev, block_idx, buf, KTFS_BLKSZ);
    if (ret != KTFS_BLKSZ) return -EIO; //ioread failed

    // copy the buffer data into the inode data structure
    memcpy(out, buf + offset, sizeof(struct ktfs_inode));
    return 0;
}

int ktfs_read_data_block(uint32_t blockno, void* buf) {
    if (!buf) return -EINVAL; // invalid arguments

    uint32_t block_idx = 1 + fs.sb.bitmap_block_count + fs.sb.inode_block_count + blockno; //index to find destred data block
    return ioreadat(fs.bdev, block_idx, buf, KTFS_BLKSZ); // read and fill into the buffer
}


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

int ktfs_open(const char * name, struct io ** ioptr) {
    // checking validity of arguments
    if (!name || !ioptr) return -EINVAL; //error, invalid aguments

    // read root inode
    struct ktfs_inode root_inode;
    int ret = ktfs_read_inode(fs.sb.root_directory_inode, &root_inode);
    if (ret < 0) return ret; //fail

    // iterate through direct data blocks to find the file
    struct ktfs_dir_entry dentries[KTFS_BLKSZ / KTFS_DENSZ]; //buffer to store entries in single data block
 

    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) { //each direct data block pointer has own data block of dentries
        if (root_inode.block[i] == 0) continue; // skipping unused blocks

        ret = ktfs_read_data_block(root_inode.block[i], dentries);
        if (ret < 0) return ret; //fail

        for (int j = 0; j < KTFS_BLKSZ / KTFS_DENSZ; j++) { // looping over each dentry

            if (strcmp(dentries[j].name, name) == 0) { //comparing the name to parsed name
                // found the file, now load its inode
                struct ktfs_inode file_inode;
                ret = ktfs_read_inode(dentries[j].inode, &file_inode); // save inode to driver
                if (ret < 0) return ret; //fail

                // allocate a ktfs_file and initialize
                struct ktfs_file *file = kcalloc(1, sizeof(struct ktfs_file));
                file->inode_num = dentries[j].inode;
                file->size = file_inode.size;
                file->flags = KTFS_FILE_IN_USE;

                // assigning the io abstraction
                static const struct iointf file_intf = {
                    .readat = ktfs_readat,
                    .cntl = ktfs_cntl,
                    .close = ktfs_close
                };

                ioinit1(&file->io, &file_intf);
                *ioptr = &file->io; // io pointer to be updated to the file io object we created
                return 0;
            }
        }
    }

    // File not found
    return -ENOENT;
}

void ktfs_close(struct io* io) {
    // checking validity of arguments
    if (!io) 
        return; 

        struct ktfs_file *file = (struct ktfs_file *)((char *)io - offsetof(struct ktfs_file, io));
        file->flags = KTFS_FILE_FREE;  // Clear the in-use flag

        // freeing the memory
        kfree(file);

    //global array curently open files 
}

long ktfs_readat(struct io* io, unsigned long long pos, void * buf, long len) {
    return 0;
}

int ktfs_cntl(struct io *io, int cmd, void *arg) {
    return 0;
}

int ktfs_flush(void) {
    struct cache *global_cache; 
    if (global_cache==0) 
    {
        return 0; 
    }
    return cache_flush(global_cache); 
    
}
