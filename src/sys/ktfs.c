// ktfs.c - KTFS implementation
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
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


// INTERNAL TYPE DEFINITIONS
//


struct ktfs_file {
    // Fill to fulfill spec
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


// INTERNAL FUNCTION DECLARATIONS
//


int ktfs_mount(struct io * io);


int ktfs_open(const char * name, struct io ** ioptr);
void ktfs_close(struct io* io);
long ktfs_readat(struct io* io, unsigned long long pos, void * buf, long len);
int ktfs_cntl(struct io *io, int cmd, void *arg);
long ktfs_writeat(struct io* io, unsigned long long pos, const void * buf, long len);

int ktfs_create(const char* name);
int ktfs_delete(const char* name);


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

int fscreate(const char* name)
    __attribute__ ((alias("ktfs_create")));

int fsdelete(const char* name)
    __attribute__ ((alias("ktfs_delete")));




// HELPER FUNCTIONS
//


// helper function to read blocks in cache
int ktfs_read_block_cached(uint32_t block_idx, void *buf) {
    void *blkptr;
    // computing byte offset of the block 
    int ret = cache_get_block(fs.cache, block_idx * KTFS_BLKSZ, &blkptr);
    // if the cache layer failed, return error
    if (ret < 0) {
        return ret;
    }
    // copying the full block from the cache into the buffer
    memcpy(buf, blkptr, KTFS_BLKSZ);
    // releasing the block back to the cache
    cache_release_block(fs.cache, blkptr, 0);
    // return the number of bytes read
    return KTFS_BLKSZ;
}


// helper to extract info from desired inode
// Inputs: uint16_t inum -it will getthe inode number to read the filesystem  
//struct ktfs_inode *out - it will point to the structure where the data will be stored
// Outputs:int - Returns 0 on success, or a negative failure
// Description: Read the inode coresponding the given inode number from the filesystem,
// calcutes the correct block and offset for the inode, and copies to the data to the output structure.
// Side Effects: Perform block read operation from the backing device
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

// helper to write an inode structure back to its slot
static int ktfs_write_inode(uint16_t inum, const struct ktfs_inode *ino) {
    // finding # of inodes per block
    uint32_t ipb     = KTFS_BLKSZ / KTFS_INOSZ;
    // finding block index of the inode
    uint32_t blk_num = 1 + fs.sb.bitmap_block_count + (inum / ipb);
    // computing byte offset in block
    uint32_t offset  = (inum % ipb) * KTFS_INOSZ;
    // getting the block from cache
    void *blkptr;
    int ret = cache_get_block(fs.cache, blk_num * KTFS_BLKSZ, &blkptr);
    if (ret < 0) {
        return ret;
    }
    // writing to the block
    memcpy((char*)blkptr + offset, ino, sizeof(*ino));
    // releasing block for writeback
    cache_release_block(fs.cache, blkptr, 1);
    return 0;
}

// helper to read whatever block we want in our filesystem image
// Inputs: uint32_t blockno -It will get the logical data block number witin the file system
//void *buf - It will point to the buffer where the data will be stored  
// Outputs: int - Returns 0 on success, or a negative failure
// Description: Read the data bloick from the file system bt calculating the location on the disk
//and copies the content of the block device.
// Side Effects: It will perform read from the block device
int ktfs_read_data_block(uint32_t blockno, void* buf) {
    if (!buf) return -EINVAL; //validating arguemnts
    uint32_t block_idx = 1 + fs.sb.bitmap_block_count + fs.sb.inode_block_count + blockno; //calculating where data blocks start
    return ktfs_read_block_cached(block_idx, buf); // reading cacheed block
}

// helper to set an inode bit in the on-disk bitmap
static int ktfs_bitmap_set(uint16_t inum) {
    // getting # of bits per block
    uint32_t bits_per_blk = KTFS_BLKSZ * 8;
    // finding bitmap block for the bit
    uint32_t bm_blk      = 1 + (inum / bits_per_blk);
    // finding bit offset in the block
    uint32_t bit_off     = inum % bits_per_blk;
    // getting the block from cache
    void *blkptr;
    int ret = cache_get_block(fs.cache, bm_blk * KTFS_BLKSZ, &blkptr);
    if (ret < 0) { 
        return ret;
    }
    // setting the bit
    ((uint8_t*)blkptr)[bit_off/8] |= (1 << (bit_off % 8));
    // releasing block for writeback
    cache_release_block(fs.cache, blkptr, 1);
    return 0;
}

// helper to bump root directory size and write back
static int ktfs_update_root_size(uint32_t delta) {
    // read current root inode
    struct ktfs_inode root_inode;
    int ret = ktfs_read_inode(fs.sb.root_directory_inode, &root_inode);
    if (ret < 0) { 
        return ret;
    }
    // incrementing root size
    root_inode.size += delta;
    return ktfs_write_inode(fs.sb.root_directory_inode, &root_inode);
}

// helper to clear a single bit (inode or data block) in the global bitmap
static int ktfs_bitmap_clear_bit(uint32_t bit_idx) {
    // getting # of bits per block
    uint32_t bits_per_blk = KTFS_BLKSZ * 8;
    // finding bitmap block for the bit
    uint32_t bm_blk      = 1 + (bit_idx / bits_per_blk);
    // finding bit offset in the block
    uint32_t bit_off     = bit_idx % bits_per_blk;
    // getting the block from cache
    void *bp;
    int ret = cache_get_block(fs.cache, bm_blk * KTFS_BLKSZ, &bp);
    if (ret < 0) return ret;
    ((uint8_t*)bp)[bit_off/8] &= ~(1 << (bit_off % 8)); // clear the bit
    cache_release_block(fs.cache, bp, 1);
    return 0;
}

// helper to decrement root directory size by delta and write back inode
static int ktfs_shrink_root(uint32_t delta) {
    struct ktfs_inode root_inode;
    int ret = ktfs_read_inode(fs.sb.root_directory_inode, &root_inode);
    if (ret < 0) return ret;
    root_inode.size -= delta; // shrink directory
    // write updated root inode
    uint32_t ipb     = KTFS_BLKSZ / KTFS_INOSZ;
    uint32_t blk_num = 1 + fs.sb.bitmap_block_count +
                       (fs.sb.root_directory_inode / ipb);
    uint32_t offset  = (fs.sb.root_directory_inode % ipb) * KTFS_INOSZ;
    void *blkptr;
    ret = cache_get_block(fs.cache, blk_num * KTFS_BLKSZ, &blkptr);
    if (ret < 0) return ret;
    memcpy((char*)blkptr + offset, &root_inode, sizeof(root_inode));
    cache_release_block(fs.cache, blkptr, 1);
    return 0;
}

// helper to allocate a free data block by scanning the bitmap
static int ktfs_alloc_data_block(uint32_t *out_blockno) {
    uint32_t bits = fs.sb.block_count; //total filesystem block count
    for (uint32_t idx = 0; idx < bits; idx++) {
        // if not in metadata, continue
        if (idx < 1 + fs.sb.bitmap_block_count + fs.sb.inode_block_count)
            continue;
        uint32_t dblk = idx;
        uint32_t data_idx = dblk - (1 + fs.sb.bitmap_block_count + fs.sb.inode_block_count);
        uint32_t bit_idx = idx;
        uint32_t bits_per_blk = KTFS_BLKSZ * 8;
        uint32_t bm_blk = 1 + (bit_idx / bits_per_blk);
        uint32_t off = bit_idx % bits_per_blk;
        void *bp;
        int ret = cache_get_block(fs.cache, bm_blk * KTFS_BLKSZ, &bp);
        if (ret < 0) return ret;
        uint8_t byte = ((uint8_t *)bp)[off/8];
        if (!(byte & (1 << (off % 8)))) {
            // found a free bit, mark it used
            ((uint8_t *)bp)[off/8] |= (1 << (off % 8));
            cache_release_block(fs.cache, bp, 1);
            *out_blockno = data_idx;
            return 0;
        }
        cache_release_block(fs.cache, bp, 0);
    }
    return -ENODATABLKS;
}

// helper to grow a file
static int ktfs_set_end(struct ktfs_file *file, unsigned long long new_end) {
    struct ktfs_inode inode;
    int ret = ktfs_read_inode(file->inode_num, &inode);
    if (ret < 0) return ret;
    // finding required new blocks
    unsigned int old_size = inode.size;
    unsigned int old_blocks = (old_size + KTFS_BLKSZ - 1) / KTFS_BLKSZ;
    unsigned int new_blocks = (new_end + KTFS_BLKSZ - 1) / KTFS_BLKSZ;
    // allocate new data blocks (direct only)
    for (unsigned i = old_blocks; i < new_blocks; i++) {
        if (i >= KTFS_NUM_DIRECT_DATA_BLOCKS) return -ENODATABLKS;
        uint32_t blkno;
        ret = ktfs_alloc_data_block(&blkno);
        if (ret < 0) return ret;
        inode.block[i] = blkno;
    }
    // updating inode size
    inode.size = new_end;
    // writing inode back
    ret = ktfs_write_inode(file->inode_num, &inode);
    if (ret < 0) return ret;
    // updating file size
    file->size = new_end;
    return 0;
}

// helper to free all blocks held by an inode
static int ktfs_free_inode_blocks(uint16_t inum) {
    struct ktfs_inode inode;
    int ret = ktfs_read_inode(inum, &inode);
    if (ret < 0) return ret;

    // compute global block offset for data blocks
    uint32_t data_base = 1 + fs.sb.bitmap_block_count + fs.sb.inode_block_count;
    uint32_t ptrs = KTFS_BLKSZ / sizeof(uint32_t);

    // freeing direct blocks
    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
        if (inode.block[i]) {
            ret = ktfs_bitmap_clear_bit(data_base + inode.block[i]);
            if (ret < 0) return ret;
        }
    }

    // freeing single-indirect blocks
    if (inode.indirect) {
        uint32_t idxs[ptrs];
        ret = ktfs_read_data_block(inode.indirect, idxs);
        if (ret < 0) return ret;
        for (uint32_t i = 0; i < ptrs && idxs[i]; i++) {
            ret = ktfs_bitmap_clear_bit(data_base + idxs[i]);
            if (ret < 0) return ret;
        }
        // then free the indirect block itself
        ret = ktfs_bitmap_clear_bit(data_base + inode.indirect);
        if (ret < 0) return ret;
    }

    // freeing doubly-indirect blocks
    for (int d = 0; d < KTFS_NUM_DINDIRECT_BLOCKS; d++) {
        uint32_t d1_idxs[ptrs];
        if (!inode.dindirect[d]) continue;
        ret = ktfs_read_data_block(inode.dindirect[d], d1_idxs);
        if (ret < 0) return ret;
        for (uint32_t i = 0; i < ptrs && d1_idxs[i]; i++) {
            uint32_t d2_idxs[ptrs];
            ret = ktfs_read_data_block(d1_idxs[i], d2_idxs);
            if (ret < 0) return ret;
            for (uint32_t j = 0; j < ptrs && d2_idxs[j]; j++) {
                ret = ktfs_bitmap_clear_bit(data_base + d2_idxs[j]);
                if (ret < 0) return ret;
            }
            // free this level-1 indirect block
            ret = ktfs_bitmap_clear_bit(data_base + d1_idxs[i]);
            if (ret < 0) return ret;
        }
        // free the top-level doubly-indirect block
        ret = ktfs_bitmap_clear_bit(data_base + inode.dindirect[d]);
        if (ret < 0) return ret;
    }
    return 0;
}


// getting block number for position reading/writing
// Inputs:  struct ktfs_inode *inode - this will point to the inode represention the file
//uint32_t file_block_index - it wioll get the index of the data block within the file
// uint32_t *out_blockno - it will output the pointer that will be set to the physical block number
// Outputs: int - Returns 0 on success, or a negative failure
// Description: it will reslove the logical fule block index to the nlock number of the disk/
//The function support the three level of the blokc lookup. It read one or more indrect bock as nessurart
//and store the resulitng block number.
// Side Effects: It will perform disk I/O read to fetch indrect of the block content
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
// EXPORTED FUNCTION DEFINITIONS
// Inputs: struct io *io - it will point to the I/O intrerface representation the backing storage device
// Outputs: int - Returns 0 on success, or a negative failure
// Description: Mounts the file system by intializingg internal structure, acquiring the backing device,
// and reading the superblock. It validates the superblock fields to ensure the file system is properly formatted
//beofore completing the mount.
// Side Effects: Acquires and release global file system lock, store refrence to the backing device,
//read from the disk and modifies the global fs structure


int ktfs_mount(struct io * io) {
    // validating arguements
    if (!io) return -EINVAL;
    // initilizing lock
    lock_init(&fs.fs_lock);
    // at reference and store into struct
    fs.bdev = ioaddref(io);
    // create the cache here
    int rc = create_cache(fs.bdev, &fs.cache);
    if (rc < 0) 
        return rc;
    // reading superblock into buffer
    static char buf[KTFS_BLKSZ];
    int ret = ioreadat(fs.bdev, 0, buf, KTFS_BLKSZ);
    if (ret != KTFS_BLKSZ) { //fail
        return -EIO;
    }
    // copying extracted superblock infor into out fs superblock struct
    memcpy(&fs.sb, buf, sizeof(struct ktfs_superblock));
    if (fs.sb.block_count == 0 || fs.sb.bitmap_block_count == 0 || fs.sb.inode_block_count == 0) {
        return -EINVAL;
    }
    return 0;
}


// Inputs: const char *name - name of the file to the opened in the file root directory
// struct io **ioptr - the output pointer that will be set to the I/O object to the opened files
// Outputs: int - Returns 0 on success, or a negative failure
// Description: Searches the root directory for a file matching to the given name. If it found read the inode allocate the file structure,
// and intializes its interface with function pointer for reading, closing, and control. Sets to the caller's ioptr to the point to this I/O object.  
// Side Effects: it may perfrom multiple block read from the backing device, allocate memory for the file structure,
//and acquires for the global file lock.
int ktfs_open(const char * name, struct io ** ioptr) {
    kprintf("ktfs_open: trying to open '%s'\n", name);
    // checking validity of arguments
    if (!name || !ioptr) return -EINVAL; //error, invalid aguments

    // // one-time cache setup once threads are running
    // static int once = 0;
    // if (!once) {
    //     int rc = create_cache(fs.bdev, &fs.cache);
    //     if (rc < 0)
    //         return rc;
    //     once = 1;
    // }

    lock_acquire(&fs.fs_lock);
    // read root inode
    struct ktfs_inode root_inode;
    int ret = ktfs_read_inode(fs.sb.root_directory_inode, &root_inode);
    if (ret < 0){
        lock_release(&fs.fs_lock); 
        return ret;
    } //fail


    // iterate through direct data blocks to find the file
    struct ktfs_dir_entry dentries[KTFS_BLKSZ / KTFS_DENSZ]; //buffer to store entries in single data block
 


    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) { //each direct data block pointer has own data block of dentries
        if (root_inode.block[i] == 0 && root_inode.block[0] != 0) continue; // skipping unused blocks


        ret = ktfs_read_data_block(root_inode.block[i], dentries);
        if (ret < 0){
            lock_release(&fs.fs_lock); 
            return ret;
        } //fail


        for (int j = 0; j < KTFS_BLKSZ / KTFS_DENSZ; j++) { // looping over each dentry


            if (strcmp(dentries[j].name, name) == 0) { //comparing the name to parsed name
                // found the file, now load its inode
                kprintf("found file\n");
                struct ktfs_inode file_inode;
                ret = ktfs_read_inode(dentries[j].inode, &file_inode); // save inode to driver
                if (ret < 0){
                    lock_release(&fs.fs_lock); 
                    return ret;
                } //fail


                // allocate a ktfs_file and initialize
                struct ktfs_file *file = kcalloc(1, sizeof(struct ktfs_file));
                file->inode_num = dentries[j].inode;
                file->size = file_inode.size;
                file->flags = KTFS_FILE_IN_USE;


                // assigning the io abstraction
                static const struct iointf file_intf = {
                    .readat = ktfs_readat,
                    .cntl = ktfs_cntl,
                    .close = ktfs_close,
                    .writeat = ktfs_writeat
                };


                ioinit1(&file->io, &file_intf);
                *ioptr = create_seekable_io(&file->io); // io pointer to be updated to the file io object we created
                lock_release(&fs.fs_lock);
                return 0;
            }
        }
    }


    // File not found
    lock_release(&fs.fs_lock);
    return -ENOENT;
}
// Inputs:  struct io *io - this will pont to the I/O object with the open files
// Outputs: None
// Description: Close to an ipen file by clearing in use flag and freeing the memory allocatedfro the file structrue
//that contain the I/O object. The I/O pointer is assumed to be created by file open .
// Side Effects: it will free dynamically allocate the memory and clear the internal flag in use
void ktfs_close(struct io* io) {
    // checking validity of arguments
    if (!io) return;


        struct ktfs_file *file = (struct ktfs_file *)((char *)io - offsetof(struct ktfs_file, io));
        file->flags = KTFS_FILE_FREE;  // Clear the in-use flag


        // freeing the memory
        kfree(file);


    //global array curently open files
}
// Inputs:struct io *io -it will point to the I/O intrerface representation the backing storage device
//unsigned long long pos - this will have the bytes offset in the file from where we start reading from
//void *buf - this will oint to the buffer where the data will be stored
//long len - the number of byte to read from the files
// Outputs: return the number of bytes read as a sucesses or a negative failure
// Description: Read up to len bytes from the open file starting at the given offset postion. It perfrom block lookup and read
//from the disk as needed and ensure that reading deos not go past the end of the file. The data is copied into the caller buffer
// Side Effects: it will perform multiple read from the disk and modifies the golbal file lock.
long ktfs_readat(struct io* io, unsigned long long pos, void * buf, long len) {
    // checking the validity of arguments
    if (!io || !buf || len < 0) return -EINVAL;


    lock_acquire(&fs.fs_lock);
   
    // retreiving file from io pointer
    struct ktfs_file *file = (struct ktfs_file *)((char *)io - offsetof(struct ktfs_file, io));

//global array curently open files
    // if file is in use then dont proceed
    if (file->flags != KTFS_FILE_IN_USE){lock_release(&fs.fs_lock); return -EINVAL;}
    if (pos >= file->size) {
        lock_release(&fs.fs_lock); 
        return 0;
    } //if position is past the filesize, dont proceed


    // block len to not read past end of file
    if (pos + len > file->size)
        len = file->size - pos;


    // read the inode for the file to extract info
    struct ktfs_inode inode;
    int ret = ktfs_read_inode(file->inode_num, &inode);
    if (ret < 0) {
        lock_release(&fs.fs_lock); 
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
        if (ret < 0){
            lock_release(&fs.fs_lock); 
            return ret;
        } //failed, return


        ret = ktfs_read_data_block(phys_blockno, blkbuf); // read from the data block (entire 512 bytes)
        if (ret != KTFS_BLKSZ){
            lock_release(&fs.fs_lock); 
            return -EIO;
        } // fail


        memcpy((char*)buf + total_read, blkbuf + block_offset, to_copy); // copy data into buffer, by "to_copy" chunks
        total_read += to_copy; // update how much we read
    }
    lock_release(&fs.fs_lock);
    return total_read; // return how much we read
}
// Inputs: struct io *io - it will pointer to the I/O object to the open file
//int cmd - it cfontrol command like IOCTL_GETBLKSZ or IOCTL_GETEND
// void *arg -it will point to the argument used by the contol command
// Outputs: int- it will return the 0 on the sucesses and write files size to argment pointer
//and return invalid argument and unsupported command
// Description: Handles control request on file I/O object. Supports  retrieving the blcok size of the file system
//and the size fo the file in the bytes to the local style commands.
// Side Effects: It write to the memory poniter to the argument if the command and acqurire and release the global file system lock.  
int ktfs_cntl(struct io *io, int cmd, void *arg) {
    if (!io) return -EINVAL;
    lock_acquire(&fs.fs_lock);
    struct ktfs_file *file = (void *)((char *)io - offsetof(struct ktfs_file, io));
    int ret = 0;

    switch (cmd) {
    case IOCTL_GETBLKSZ:
        ret = 1;
        break;
    case IOCTL_GETEND:
        if (!arg) ret = -EINVAL;
        else *(unsigned long long *)arg = file->size;
        break;
    case IOCTL_SETEND:
        if (!arg) {
            ret = -EINVAL;
        } else {
            unsigned long long new_end = *(unsigned long long *)arg;
            ret = ktfs_set_end(file, new_end);
        }
        break;
    default:
        ret = -ENOTSUP;
    }
    lock_release(&fs.fs_lock);
    return ret;
}

// Inputs: None
// Outputs: int - Returns 0 on success, or a negative failure
// Description: it is flushes all the dirty block from the cache back to the backing devic to ensure
//data. If the cache is not intiailize, the function is not not working.
// Side Effects: Acquire and release the global file system lock, may perfrom multiple write operations
//to the backing device cache flush rotine.
int ktfs_flush(void) {
    lock_acquire(&fs.fs_lock); //getting lock


    int ret = 0;
    if (fs.cache != NULL) { //if cache exists, we will flusht to device
        ret = cache_flush(fs.cache);
    }


    lock_release(&fs.fs_lock); //release lock
    return ret;
}

long ktfs_writeat(struct io *io, unsigned long long pos, const void *buf, long len) {
    if (!io || !buf || len < 0) return -EINVAL;
    lock_acquire(&fs.fs_lock);
    struct ktfs_file *file = (void *)((char *)io - offsetof(struct ktfs_file, io));
    if (file->flags != KTFS_FILE_IN_USE) {
        lock_release(&fs.fs_lock);
        return -EINVAL;
    }
    // grow file if writing past EOF
    unsigned long long end_pos = pos + len;
    if (end_pos > file->size) {
        int e2 = ktfs_set_end(file, end_pos);
        if (e2 < 0) { 
            lock_release(&fs.fs_lock); 
            return e2; 
        }
    }
    struct ktfs_inode inode;
    int ret = ktfs_read_inode(file->inode_num, &inode);
    if (ret < 0) { 
        lock_release(&fs.fs_lock); 
        return ret; 
    }
    long total = 0;
    while (total < len) {
        uint64_t cur = pos + total;
        uint32_t bidx = cur / KTFS_BLKSZ;
        uint32_t boff = cur % KTFS_BLKSZ;
        uint32_t left = len - total;
        uint32_t to = KTFS_BLKSZ - boff;
        if (to > left) to = left;
        uint32_t phys;
        ret = get_blocknum_for_offset(&inode, bidx, &phys);
        if (ret < 0) { 
            lock_release(&fs.fs_lock); 
            return ret; 
        }
        void *blk;
        uint64_t disk_off = (1 + fs.sb.bitmap_block_count + fs.sb.inode_block_count + phys) * KTFS_BLKSZ;
        ret = cache_get_block(fs.cache, disk_off, &blk);
        if (ret < 0) { 
            lock_release(&fs.fs_lock); 
            return ret; 
        }
        memcpy((char *)blk + boff, (char *)buf + total, to);
        cache_release_block(fs.cache, blk, 1);
        total += to;
    }
    lock_release(&fs.fs_lock);
    return total;
}

int ktfs_create(const char* name) {
    if (!name || strlen(name) > KTFS_MAX_FILENAME_LEN)
        return -EINVAL;

    lock_acquire(&fs.fs_lock);

    // finding root directory inode
    struct ktfs_inode root_inode;
    int ret = ktfs_read_inode(fs.sb.root_directory_inode, &root_inode);
    if (ret < 0) { 
        lock_release(&fs.fs_lock); 
        return ret; 
    }

    // if no directory block yet, allocate & zero it
    if (root_inode.block[0] == 0) {
        uint32_t new_blk;
        ret = ktfs_alloc_data_block(&new_blk);
        if (ret < 0) { 
            lock_release(&fs.fs_lock); 
            return ret; 
        }
        root_inode.block[0] = new_blk;
        ret = ktfs_write_inode(fs.sb.root_directory_inode, &root_inode);
        if (ret < 0) { 
            lock_release(&fs.fs_lock); 
            return ret; 
        }

        // zero the new block
        char zero[KTFS_BLKSZ] = {0};
        uint32_t global = 1 + fs.sb.bitmap_block_count
                        + fs.sb.inode_block_count
                        + new_blk;
        void *bp;
        ret = cache_get_block(fs.cache, global * KTFS_BLKSZ, &bp);
        if (ret < 0) { 
            lock_release(&fs.fs_lock); 
            return ret; 
        }
        memcpy(bp, zero, KTFS_BLKSZ);
        cache_release_block(fs.cache, bp, 1);
    }

    // finding free slot or detect duplicates
    struct ktfs_dir_entry dentries[KTFS_BLKSZ / KTFS_DENSZ];
    int free_idx = -1, block_idx = -1;
    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
        if (root_inode.block[i] == 0) continue;
        ret = ktfs_read_data_block(root_inode.block[i], dentries);
        if (ret < 0) { 
            lock_release(&fs.fs_lock); 
            return ret; 
        }
        for (int j = 0; j < KTFS_BLKSZ/KTFS_DENSZ; j++) {
            if (dentries[j].inode) {
                if (strcmp(dentries[j].name, name) == 0) {
                    lock_release(&fs.fs_lock);
                    return -EINVAL;
                }
            } else if (free_idx < 0) {
                free_idx = j;
                block_idx = i;
            }
        }
    }
    if (free_idx < 0) {
        lock_release(&fs.fs_lock);
        return -EINVAL;
    }

    // finding free inode
    uint32_t ipb = KTFS_BLKSZ / KTFS_INOSZ;
    uint32_t total_inodes = fs.sb.inode_block_count * ipb;
    struct ktfs_inode tmp;
    uint16_t free_inum;
    for (free_inum = 0; free_inum < total_inodes; free_inum++) {
        ret = ktfs_read_inode(free_inum, &tmp);
        if (ret == 0 && tmp.flags == 0) break;
    }
    if (free_inum >= total_inodes) {
        lock_release(&fs.fs_lock);
        return -ENOINODEBLKS;
    }

    // updating inode in bitmap
    ret = ktfs_bitmap_set(free_inum);
    if (ret < 0) { 
        lock_release(&fs.fs_lock); 
        return ret; 
    }

    // initializing & writing new inode
    struct ktfs_inode new_inode = {0};
    new_inode.flags = KTFS_FILE_IN_USE;
    ret = ktfs_write_inode(free_inum, &new_inode);
    if (ret < 0) { 
        lock_release(&fs.fs_lock); 
        return ret; 
    }

    // inserting directory entry
    ret = ktfs_read_data_block(root_inode.block[block_idx], dentries);
    if (ret < 0) { 
        lock_release(&fs.fs_lock); 
        return ret; 
    }
    strncpy(dentries[free_idx].name, name, KTFS_MAX_FILENAME_LEN);
    dentries[free_idx].name[KTFS_MAX_FILENAME_LEN] = '\0';
    dentries[free_idx].inode = free_inum;

    // writing back updated directory block
    uint32_t dblk = 1 + fs.sb.bitmap_block_count + fs.sb.inode_block_count + root_inode.block[block_idx];
    void *dptr;
    ret = cache_get_block(fs.cache, dblk * KTFS_BLKSZ, &dptr);
    if (ret < 0) { 
        lock_release(&fs.fs_lock); 
        return ret; 
    }
    memcpy(dptr, dentries, KTFS_BLKSZ);
    cache_release_block(fs.cache, dptr, 1);

    // updating root directory size
    ret = ktfs_update_root_size(KTFS_DENSZ);
    if (ret < 0) { 
        lock_release(&fs.fs_lock); 
        return ret; 
    }

    lock_release(&fs.fs_lock);
    return 0;
}

int ktfs_delete(const char* name) {
    if (!name || strlen(name) > KTFS_MAX_FILENAME_LEN)
        return -EINVAL;
    lock_acquire(&fs.fs_lock);

    // finding root directory inode
    struct ktfs_inode root_inode;
    int ret = ktfs_read_inode(fs.sb.root_directory_inode, &root_inode);
    if (ret < 0) { 
        lock_release(&fs.fs_lock); 
        return ret; 
    }

    // finding directory entry
    struct ktfs_dir_entry dentries[KTFS_BLKSZ / KTFS_DENSZ];
    int block_idx = -1, entry_idx = -1;
    uint16_t target_inum = 0;
    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
        if (!root_inode.block[i]) continue;
        ret = ktfs_read_data_block(root_inode.block[i], dentries);
        if (ret < 0) { 
            lock_release(&fs.fs_lock); 
            return ret; 
        }
        for (int j = 0; j < KTFS_BLKSZ/KTFS_DENSZ; j++) {
            if (!dentries[j].inode) break;
            if (strcmp(dentries[j].name, name) == 0) {
                block_idx   = i;
                entry_idx   = j;
                target_inum = dentries[j].inode;
                break;
            }
        }
        if (block_idx >= 0) break;
    }
    if (block_idx < 0) {
        lock_release(&fs.fs_lock);
        return -ENOENT;
    }

    // freeing all of the file's data blocks
    ret = ktfs_free_inode_blocks(target_inum);
    if (ret < 0) { 
        lock_release(&fs.fs_lock); 
        return ret; 
    }

    // clearing and writing back file inode
    struct ktfs_inode zero = {0};
    uint32_t ipb     = KTFS_BLKSZ / KTFS_INOSZ;
    uint32_t blk_num = 1 + fs.sb.bitmap_block_count + (target_inum / ipb);
    uint32_t offset  = (target_inum % ipb) * KTFS_INOSZ;
    void *blkptr;
    ret = cache_get_block(fs.cache, blk_num * KTFS_BLKSZ, &blkptr);
    if (ret < 0) { 
        lock_release(&fs.fs_lock); 
        return ret; 
    }
    memcpy((char*)blkptr + offset, &zero, sizeof(zero));
    cache_release_block(fs.cache, blkptr, 1);

    // clearing inode's bitmap bit
    ret = ktfs_bitmap_clear_bit(target_inum);
    if (ret < 0) { 
        lock_release(&fs.fs_lock); 
        return ret; 
    }

    // shifting directory entries
    ret = ktfs_read_data_block(root_inode.block[block_idx], dentries);
    if (ret < 0) { lock_release(&fs.fs_lock); return ret; }
    for (int k = entry_idx; k + 1 < KTFS_BLKSZ/KTFS_DENSZ; k++) {
        dentries[k] = dentries[k+1];
    }
    memset(&dentries[(KTFS_BLKSZ/KTFS_DENSZ)-1], 0, sizeof(dentries[0]));

    // writing back updated directory block
    uint32_t dblk = 1 + fs.sb.bitmap_block_count + fs.sb.inode_block_count + root_inode.block[block_idx];
    void *dptr;
    ret = cache_get_block(fs.cache, dblk * KTFS_BLKSZ, &dptr);
    if (ret < 0) { 
        lock_release(&fs.fs_lock); 
        return ret; 
    }
    memcpy(dptr, dentries, KTFS_BLKSZ);
    cache_release_block(fs.cache, dptr, 1);

    // shrinking root directory and writing back
    ret = ktfs_shrink_root(KTFS_DENSZ);
    if (ret < 0) { 
        lock_release(&fs.fs_lock);
        return ret;
    }

    lock_release(&fs.fs_lock);
    return 0;
}