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
    struct lock fs_lock;   // added lock implementation
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

// helper funciton to read blocks in cache
int ktfs_read_block_cached(uint32_t block_idx, void *buf) {
    void *blkptr;
    int ret = cache_get_block(fs.cache, block_idx * KTFS_BLKSZ, &blkptr); // getting blcok in cache
    if (ret < 0) return ret; // fail 
    memcpy(buf, blkptr, KTFS_BLKSZ); //copy into buffer
    cache_release_block(fs.cache, blkptr, 0); //releasing cache block
    return KTFS_BLKSZ;
}

// helper to extract info from desired inode
int ktfs_read_inode(uint16_t inum, struct ktfs_inode *out) {
    if (!out) return -EINVAL; //validating arguemtns
    const int inodes_per_block = KTFS_BLKSZ / KTFS_INOSZ; // calculating inodes inside one block
    uint32_t block_idx = 1 + fs.sb.bitmap_block_count + (inum / inodes_per_block); // inode indexes start here
    uint32_t offset = (inum % inodes_per_block) * KTFS_INOSZ; //offset inside the desired inode block
    char buf[KTFS_BLKSZ]; // buffer to fill
    int ret = ktfs_read_block_cached(block_idx, buf); //reading cached block 
    if (ret != KTFS_BLKSZ) return -EIO; //fail
    memcpy(out, buf + offset, sizeof(struct ktfs_inode)); //copying into out buffer
    return 0;
}

// helper to read whatever block we want in our filesystem image
int ktfs_read_data_block(uint32_t blockno, void* buf) {
    if (!buf) return -EINVAL; //validating arguemnts
    uint32_t block_idx = 1 + fs.sb.bitmap_block_count + fs.sb.inode_block_count + blockno; //calculating where data blocks start
    return ktfs_read_block_cached(block_idx, buf); // reading cacheed block
}

// getting block number for position reading/writing
int get_blocknum_for_offset(struct ktfs_inode *inode, uint32_t file_block_index, uint32_t *out_blockno) {
    if (!inode || !out_blockno) return -EINVAL; // validating arguements
    const uint32_t ptrs_per_block = KTFS_BLKSZ / POINTER_BYTESIZE; // calculating the number of pointers per block

    if (file_block_index < KTFS_NUM_DIRECT_DATA_BLOCKS) { // if the index is less then number of direct datablocks,
        *out_blockno = inode->block[file_block_index]; //direct block points to this our block
        return (*out_blockno != 0) ? 0 : -ENOENT;//return outblock if it exists
    }

    file_block_index -= KTFS_NUM_DIRECT_DATA_BLOCKS; //update index for indirect blocks

    if (file_block_index < ptrs_per_block) { //if index actually within bounds
        if (inode->indirect == 0) return -ENOENT; //check if its actually pointing at something
        uint32_t indirect_block[ptrs_per_block];
        int ret = ktfs_read_data_block(inode->indirect, indirect_block); //reading the desited inode
        if (ret != KTFS_BLKSZ) return -EIO;
        *out_blockno = indirect_block[file_block_index]; // pointer is now directly pointing to block
        return (*out_blockno != 0) ? 0 : -ENOENT;
    }

    file_block_index -= ptrs_per_block; //updating the index
    const uint32_t blocks_per_dindirect = ptrs_per_block * ptrs_per_block;

    for (int i = 0; i < KTFS_NUM_DINDIRECT_BLOCKS; i++) {
        if (file_block_index < blocks_per_dindirect) { //bounds checking
            if (inode->dindirect[i] == 0) return -ENOENT;
            uint32_t level1[ptrs_per_block]; //checking doubly indirect pointers
            int ret = ktfs_read_data_block(inode->dindirect[i], level1); //reading first level of block
            if (ret != KTFS_BLKSZ) return -EIO;
            uint32_t l1_index = file_block_index / ptrs_per_block; //firest block check index
            uint32_t l2_index = file_block_index % ptrs_per_block; //second block check index
            if (level1[l1_index] == 0) return -ENOENT; //if first doesnt point to anything retrun
            uint32_t level2[ptrs_per_block]; //checking seconde pointer, which should be direct
            ret = ktfs_read_data_block(level1[l1_index], level2); //read that block
            if (ret != KTFS_BLKSZ) return -EIO;
            *out_blockno = level2[l2_index]; //set out block to the block we found
            return (*out_blockno != 0) ? 0 : -ENOENT;
        }
        file_block_index -= blocks_per_dindirect;
    }
    return -ENOENT;
}

int ktfs_mount(struct io * io) {
    // validating arguements
    if (!io) return -EINVAL;
    // initilizing and aqcuring lock
    lock_init(&fs.fs_lock);
    lock_acquire(&fs.fs_lock);
    // at reference and store into struct
    fs.bdev = ioaddref(io);
    // allocating cache
    if (fs.cache == NULL) {
        int rc = create_cache(fs.bdev, &fs.cache);
        if (rc < 0) {
            lock_release(&fs.fs_lock); //releasing lock before return
            return rc;
        }
    }
    // reading superblock into buffer
    static char buf[KTFS_BLKSZ];
    int ret = ioreadat(fs.bdev, 0, buf, KTFS_BLKSZ);
    if (ret != KTFS_BLKSZ) { //fail
        lock_release(&fs.fs_lock);
        return -EIO;
    }
    // copying extracted superblock infor into out fs superblock struct
    memcpy(&fs.sb, buf, sizeof(struct ktfs_superblock)); 
    if (fs.sb.block_count == 0 || fs.sb.bitmap_block_count == 0 || fs.sb.inode_block_count == 0) {
        lock_release(&fs.fs_lock); //validating superblock
        return -EINVAL;
    }
    lock_release(&fs.fs_lock); //releasing lock
    return 0;
}


int ktfs_open(const char * name, struct io ** ioptr) {
    // checking validity of arguments
    if (!name || !ioptr) return -EINVAL; //error, invalid aguments

    lock_acquire(&fs.fs_lock);
    // read root inode
    struct ktfs_inode root_inode;
    int ret = ktfs_read_inode(fs.sb.root_directory_inode, &root_inode);
    if (ret < 0){lock_release(&fs.fs_lock); return ret;} //fail

    // iterate through direct data blocks to find the file
    struct ktfs_dir_entry dentries[KTFS_BLKSZ / KTFS_DENSZ]; //buffer to store entries in single data block
 

    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) { //each direct data block pointer has own data block of dentries
        if (root_inode.block[i] == 0 && root_inode.block[0] != 0) continue; // skipping unused blocks

        ret = ktfs_read_data_block(root_inode.block[i], dentries);
        if (ret < 0){lock_release(&fs.fs_lock); return ret;} //fail

        for (int j = 0; j < KTFS_BLKSZ / KTFS_DENSZ; j++) { // looping over each dentry

            if (strcmp(dentries[j].name, name) == 0) { //comparing the name to parsed name
                // found the file, now load its inode
                struct ktfs_inode file_inode;
                ret = ktfs_read_inode(dentries[j].inode, &file_inode); // save inode to driver
                if (ret < 0){lock_release(&fs.fs_lock); return ret;} //fail

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
                lock_release(&fs.fs_lock);
                return 0;
            }
        }
    }

    // File not found
    lock_release(&fs.fs_lock);
    return -ENOENT;
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

    lock_acquire(&fs.fs_lock);
    
    // retreiving file from io pointer
    struct ktfs_file *file = (struct ktfs_file *)((char *)io - offsetof(struct ktfs_file, io));

    // if file is in use then dont proceed
    if (file->flags != KTFS_FILE_IN_USE){lock_release(&fs.fs_lock); return -EINVAL;}
    if (pos >= file->size) {lock_release(&fs.fs_lock); return 0;} //if position is past the filesize, dont proceed

    // block len to not read past end of file
    if (pos + len > file->size)
        len = file->size - pos;

    // read the inode for the file to extract info
    struct ktfs_inode inode;
    int ret = ktfs_read_inode(file->inode_num, &inode);
    if (ret < 0) {lock_release(&fs.fs_lock); return ret;}

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
        if (ret < 0){lock_release(&fs.fs_lock); return ret;} //failed, return

        ret = ktfs_read_data_block(phys_blockno, blkbuf); // read from the data block (entire 512 bytes)
        if (ret != KTFS_BLKSZ){lock_release(&fs.fs_lock); return -EIO;} // fail

        memcpy((char*)buf + total_read, blkbuf + block_offset, to_copy); // copy data into buffer, by "to_copy" chunks
        total_read += to_copy; // update how much we read
    }
    lock_release(&fs.fs_lock);
    return total_read; // return how much we read
}

int ktfs_cntl(struct io *io, int cmd, void *arg) {
    if (!io) return -EINVAL;
    lock_acquire(&fs.fs_lock);

    struct ktfs_file *file = (struct ktfs_file *)((char *)io - offsetof(struct ktfs_file, io)); // this will ge tthe parent file structure from the io pointer 

    if (cmd == IOCTL_GETBLKSZ) {  //check if the ge the block size 
        lock_release(&fs.fs_lock);
        return KTFS_BLKSZ;  // will get the block size 

    } 
    
    else if (cmd == IOCTL_GETEND){ //check if the command get the aize of the file in bytes 
        if (!arg){lock_release(&fs.fs_lock); return -EINVAL;}
        *(unsigned long long *)arg = file->size; //thsi will write the file size to the location by the argument  
        lock_release(&fs.fs_lock);
        return 0;
    } 
    
    
    else {lock_release(&fs.fs_lock); return -ENOTSUP;}  // nthis is usupported control command
}

int ktfs_flush(void) { 
    lock_acquire(&fs.fs_lock); //getting lock

    int ret = 0;
    if (fs.cache != NULL) { //if cache exists, we will flusht to device
        ret = cache_flush(fs.cache);
    }

    lock_release(&fs.fs_lock); //release lock
    return ret;
}