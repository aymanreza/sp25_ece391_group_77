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
    struct cache *cache;
} fs;

// declaring global filesystem lock
static struct lock fs_lock;

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

int get_blocknum_for_offset(struct ktfs_inode *inode, uint32_t file_block_index, uint32_t *out_blockno) {
    // checking the validity of arguments
    if (!inode || !out_blockno)
        return -EINVAL;

    const uint32_t ptrs_per_block = KTFS_BLKSZ / POINTER_BYTESIZE;  // 128 pointers per block

    // DIRECTBLOCK
    if (file_block_index < KTFS_NUM_DIRECT_DATA_BLOCKS) { // cycling through the direct blocks
        *out_blockno = inode->block[file_block_index];
        return (*out_blockno != 0) ? 0 : -ENOENT;
    }

    // Adjust for indirect
    file_block_index -= KTFS_NUM_DIRECT_DATA_BLOCKS;

    // INDIRECT BLOCK
    if (file_block_index < ptrs_per_block) { //cycling through the indirect blocks
        if (inode->indirect == 0)
            return -ENOENT;

        uint32_t indirect_block[ptrs_per_block];
        int ret = ktfs_read_data_block(inode->indirect, indirect_block);
        if (ret != KTFS_BLKSZ) return -EIO;

        *out_blockno = indirect_block[file_block_index];
        return (*out_blockno != 0) ? 0 : -ENOENT;
    }

    // Adjust for doubly indirect
    file_block_index -= ptrs_per_block;

    // DOUBLY INDIRECT BLOCKS
    const uint32_t blocks_per_dindirect = ptrs_per_block * ptrs_per_block;

    for (int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS; i++) { //cycling through the doubly indirect blocks
        if (file_block_index < blocks_per_dindirect) {
            if (inode->dindirect[i] == 0)
                return -ENOENT;

            uint32_t level1[ptrs_per_block];
            int ret = ktfs_read_data_block(inode->dindirect[i], level1);
            if (ret != KTFS_BLKSZ) return -EIO;

            uint32_t l1_index = file_block_index / ptrs_per_block;
            uint32_t l2_index = file_block_index % ptrs_per_block;

            if (level1[l1_index] == 0)
                return -ENOENT;

            uint32_t level2[ptrs_per_block];
            ret = ktfs_read_data_block(level1[l1_index], level2);
            if (ret != KTFS_BLKSZ) return -EIO;

            *out_blockno = level2[l2_index];
            return (*out_blockno != 0) ? 0 : -ENOENT;
        }

        file_block_index -= blocks_per_dindirect;
    }

    // File too large
    return -ENOENT;
}


// EXPORTED FUNCTION DEFINITIONS
//

int ktfs_mount(struct io * io) {
    // checking validity of io argument
    if (!io) 
        return -EINVAL;

    lock_init(&fs_lock); // initializing filesystem lock

    lock_acquire(&fs_lock); // acquiring fs lock before copying from buf

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

    lock_release(&fs_lock); // releasing filesystem lock before return

    // return success
    return 0;
}

int ktfs_open(const char * name, struct io ** ioptr) {
    // checking validity of arguments
    if (!name || !ioptr) return -EINVAL; //error, invalid aguments

    // acquiring the filesystem lock
    lock_acquire(%fs_lock);

    // read root inode
    struct ktfs_inode root_inode;
    int ret = ktfs_read_inode(fs.sb.root_directory_inode, &root_inode);
    if (ret < 0) {
        lock_release(&fs_lock); // releasing filesystem lock before ret
        return ret; //fail
    }

    // iterate through direct data blocks to find the file
    struct ktfs_dir_entry dentries[KTFS_BLKSZ / KTFS_DENSZ]; //buffer to store entries in single data block
 

    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) { //each direct data block pointer has own data block of dentries
        if (root_inode.block[i] == 0) continue; // skipping unused blocks

        ret = ktfs_read_data_block(root_inode.block[i], dentries);
        if (ret < 0) {
            lock_release(&fs_lock); // releasing filesystem lock before ret
            return ret; // fail
        }

        for (int j = 0; j < KTFS_BLKSZ / KTFS_DENSZ; j++) { // looping over each dentry

            if (strcmp(dentries[j].name, name) == 0) { //comparing the name to parsed name
                // found the file, now load its inode
                struct ktfs_inode file_inode;
                ret = ktfs_read_inode(dentries[j].inode, &file_inode); // save inode to driver
                if (ret < 0) {
                    lock_release(&fs_lock); // releasing filesystem lock before ret
                    return ret; //fail
                }

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
                lock_release(&fs_lock); // releasing filesystem lock before return
                return 0;
            }
        }
    }
    lock_release(&fs_lock); // releasing filesystem lock before error
    return -ENOENT; // File not found
}

void ktfs_close(struct io* io) {
    // checking validity of arguments
    if (!io) return; 

        struct ktfs_file *file = (struct ktfs_file *)((char *)io - offsetof(struct ktfs_file, io));
        file->flags = KTFS_FILE_FREE;  // Clear the in-use flag

        // freeing the memory
        kfree(file);

    //global array curently open files 
}

long ktfs_readat(struct io* io, unsigned long long pos, void * buf, long len) {
    // checking the validity of arguments
    if (!io || !buf || len < 0) return -EINVAL;

    // acquiring filesystem lock before read operations
    lock_acquire(&fs_lock);
    
    // retreiving file from io pointer
    struct ktfs_file *file = (struct ktfs_file *)((char *)io - offsetof(struct ktfs_file, io));

    // if file is in use then dont proceed
    if (file->flags != KTFS_FILE_IN_USE) {
        lock_release(&fs_lock); // releasing filesystem lock before error
        return -EINVAL;
    }
    if (pos >= file->size) {
        lock_release(&fs_lock); // releasing filesystem lock before return
        return 0; //if position is past the filesize, dont proceed
    }

    // block len to not read past end of file
    if (pos + len > file->size)
        len = file->size - pos;

    // read the inode for the file to extract info
    struct ktfs_inode inode;
    int ret = ktfs_read_inode(file->inode_num, &inode);
    if (ret < 0) {
        lock_release(&fs_lock); // releasing filesystem lock before ret
        return ret;
    }

    // create out buffer for read
    char blkbuf[KTFS_BLKSZ];
    long total_read = 0;

    while (total_read < len) {
        // update the position adter each read
        uint64_t cur_pos = pos + total_read;
        uint32_t block_idx = cur_pos / KTFS_BLKSZ; // which data block we want to read from
        uint32_t block_offset = cur_pos % KTFS_BLKSZ; // where in the data block we want to read
        uint32_t bytes_left = len - total_read; // how many bytes left to read
        uint32_t to_copy = KTFS_BLKSZ - block_offset; // how many bytes to read in current block

        if (to_copy > bytes_left) // if there are less bytes to read than
            to_copy = bytes_left; // bytes from the offset to the end, tocopy = bytes_left

        uint32_t phys_blockno;
        ret = get_blocknum_for_offset(&inode, block_idx, &phys_blockno); // retriece block number of where data is in file
        if (ret < 0) {
            lock_release(&fs_lock); // releasing filesystem lock before ret
            return ret; //failed, return
        }

        ret = ktfs_read_data_block(phys_blockno, blkbuf); // read from the data block (entire 512 bytes)
        if (ret != KTFS_BLKSZ) {
            lock_release(&fs_lock); // releasing filesystem lock before error
            return -EIO; // fail
        }

        memcpy((char*)buf + total_read, blkbuf + block_offset, to_copy); // copy data into buffer, by "to_copy" chunks
        total_read += to_copy; // update how much we read
    }
    lock_release(&fs_lock); // releasing filesystem lock before read returns
    return total_read; // return how much we read
}

int ktfs_cntl(struct io *io, int cmd, void *arg) {
    if (!io) return -EINVAL;

    struct ktfs_file *file = (struct ktfs_file *)((char *)io - offsetof(struct ktfs_file, io)); // this will ge tthe parent file structure from the io pointer 

    if (cmd == IOCTL_GETBLKSZ) {  //check if the ge the block size 
        return KTFS_BLKSZ;  // will get the block size 

    } 
    
    else if (cmd == IOCTL_GETEND){ //check if the command get the aize of the file in bytes 
        if (!arg) return -EINVAL;
        *(unsigned long long *)arg = file->size; //thsi will write the file size to the location by the argument  
        return 0;
    } 
    
    else return -ENOTSUP;  // nthis is usupported control command
}

int ktfs_flush(void) { 
    lock_acquire(&fs_lock); // acquiring lock before flushing cache
    int ret = 0;
    if (fs.cache == NULL) {
        ret = 0;
    } 
    else {
        ret = cache_flush(fs.cache);
    } 
    lock_release(&fs_lock); // releasing lock before return
    return ret;
}