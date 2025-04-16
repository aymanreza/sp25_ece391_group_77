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


// Inputs: const char *name - name of the file to the opened in the file root directory
// struct io **ioptr - the output pointer that will be set to the I/O object to the opened files
// Outputs: int - Returns 0 on success, or a negative failure
// Description: Searches the root directory for a file matching to the given name. If it found read the inode allocate the file structure,
// and intializes its interface with function pointer for reading, closing, and control. Sets to the caller's ioptr to the point to this I/O object.  
// Side Effects: it may perfrom multiple block read from the backing device, allocate memory for the file structure,
//and acquires for the global file lock.
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


long ktfs_writeat(struct io* io, unsigned long long pos, const void * buf, long len)
{
     if (!io || !buf || len < 0) return -EINVAL;
     lock_acquire(&fs.fs_lock);
     struct ktfs_file *file = (struct ktfs_file *)((char *)io - offsetof(struct ktfs_file, io));
     if (file->flags != KTFS_FILE_IN_USE) {
         lock_release(&fs.fs_lock);
         return -EINVAL;
     }
     if (pos >= file->size) {
         lock_release(&fs.fs_lock);
         return 0;  
     }
     if (pos + len > file->size) {
         len = file->size - pos;
     }
     struct ktfs_inode inode;
     int ret = ktfs_read_inode(file->inode_num, &inode);
     if (ret < 0) {
         lock_release(&fs.fs_lock);
         return ret;
     }
     long total_written = 0;
     while (total_written < len) {
         uint64_t cur_pos = pos + total_written;
         uint32_t block_idx = cur_pos / KTFS_BLKSZ;
         uint32_t block_offset = cur_pos % KTFS_BLKSZ;
         uint32_t bytes_left = len - total_written;
         uint32_t to_write = KTFS_BLKSZ - block_offset;
 
         if (to_write > bytes_left)
             to_write = bytes_left;
 
         uint32_t phys_blockno;
         ret = get_blocknum_for_offset(&inode, block_idx, &phys_blockno);
         if (ret < 0) {
             lock_release(&fs.fs_lock);
             return ret;
         }
         void *blkptr;
         ret = cache_get_block(fs.cache, (1 + fs.sb.bitmap_block_count + fs.sb.inode_block_count + phys_blockno) * KTFS_BLKSZ, &blkptr);
         if (ret < 0) {
             lock_release(&fs.fs_lock);
             return ret;
         }
         memcpy((char *)blkptr + block_offset, (char *)buf + total_written, to_write);
         cache_release_block(fs.cache, blkptr, 1);
 
         total_written += to_write;
     }
     lock_release(&fs.fs_lock);long ktfs_writeat(struct io* io, unsigned long long pos, const void * buf, long len);


     return total_written;
}
int ktfs_create(const char* name)
{
    if (!name || strlen(name) > KTFS_MAX_FILENAME_LEN)
        return -EINVAL;


    lock_acquire(&fs.fs_lock);


    struct ktfs_inode root_inode;
    int ret = ktfs_read_inode(fs.sb.root_directory_inode, &root_inode);
    if (ret < 0) {    lock_acquire(&fs.fs_lock);


        lock_release(&fs.fs_lock);
        return ret;
    }


    struct ktfs_dir_entry dentries[KTFS_BLKSZ / KTFS_DENSZ];
    int free_idx = -1;
    int block_idx = -1;
    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
        if (root_inode.block[i] == 0) continue;


        ret = ktfs_read_data_block(root_inode.block[i], dentries);
        if (ret < 0) {
            lock_release(&fs.fs_lock);
            return ret;
        }


        for (int j = 0; j < KTFS_BLKSZ / KTFS_DENSZ; j++) {
            if (dentries[j].inode != 0) {
                if (strncmp(dentries[j].name, name, KTFS_MAX_FILENAME_LEN) == 0) {
                    lock_release(&fs.fs_lock);
                    return -1;  
                }
            } else if (free_idx == -1) {
               
                free_idx = j;
                block_idx = i;
            }
        }
    }


    if (free_idx == -1 || block_idx == -1) {
        lock_release(&fs.fs_lock);
        return -1;  
    }


 
    uint32_t inodes_per_block = KTFS_BLKSZ / KTFS_INOSZ;
    uint32_t total_inodes = fs.sb.inode_block_count * inodes_per_block;
    struct ktfs_inode temp;
    uint16_t free_inum = 0;
    int found_inode = 0;


    for (free_inum = 0; free_inum < total_inodes; free_inum++) {
        ret = ktfs_read_inode(free_inum, &temp);
        if (ret == 0 && temp.flags == 0)
        {  
            found_inode = 1;
            break;
        }
    }


    if (!found_inode) {
        lock_release(&fs.fs_lock);
        return -ENOINODEBLKS;  
    }


   
    struct ktfs_inode new_inode = {0};
    new_inode.size = 0;
    new_inode.flags = KTFS_FILE_IN_USE;


    uint32_t blk_num = 1 + fs.sb.bitmap_block_count + (free_inum / inodes_per_block);
    uint32_t offset = (free_inum % inodes_per_block) * KTFS_INOSZ;


    void* blkptr;
    ret = cache_get_block(fs.cache, blk_num * KTFS_BLKSZ, &blkptr);
    if (ret < 0) {
        lock_release(&fs.fs_lock);
        return ret;
    }


    memcpy((char*)blkptr + offset, &new_inode, sizeof(struct ktfs_inode));
    cache_release_block(fs.cache, blkptr, 1);  


    ret = ktfs_read_data_block(root_inode.block[block_idx], dentries);
    if (ret < 0) {
        lock_release(&fs.fs_lock);
        return ret;
    }


    strncpy(dentries[free_idx].name, name, KTFS_MAX_FILENAME_LEN);
    dentries[free_idx].name[KTFS_MAX_FILENAME_LEN] = '\0';
    dentries[free_idx].inode = free_inum;


   
    uint32_t dentry_blk_idx = 1 + fs.sb.bitmap_block_count + fs.sb.inode_block_count + root_inode.block[block_idx];
    ret = cache_get_block(fs.cache, dentry_blk_idx * KTFS_BLKSZ, &blkptr);
    if (ret < 0) {
        lock_release(&fs.fs_lock);
        return ret;
    }


    memcpy(blkptr, dentries, KTFS_BLKSZ);
    cache_release_block(fs.cache, blkptr, 1);  


    lock_release(&fs.fs_lock);
    return 0;
}


int ktfs_delete(const char* name)
{
    if (!name || strlen(name) > KTFS_MAX_FILENAME_LEN)
    return -EINVAL;


lock_acquire(&fs.fs_lock);


struct ktfs_inode root_inode;
int ret = ktfs_read_inode(fs.sb.root_directory_inode, &root_inode);
if (ret < 0) {
    lock_release(&fs.fs_lock);
    return ret;
}


struct ktfs_dir_entry dentries[KTFS_BLKSZ / KTFS_DENSZ];
int block_idx = -1, entry_idx = -1;
uint16_t target_inode = 0;


for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++) {
    if (root_inode.block[i] == 0) continue;


    ret = ktfs_read_data_block(root_inode.block[i], dentries);
    if (ret < 0) {
        lock_release(&fs.fs_lock);
        return ret;
    }


    for (int j = 0; j < KTFS_BLKSZ / KTFS_DENSZ; j++) {
        if (strncmp(dentries[j].name, name, KTFS_MAX_FILENAME_LEN) == 0) {
            block_idx = i;
            entry_idx = j;
            target_inode = dentries[j].inode;
            goto DELETE_FOUND;
        }
    }
}


lock_release(&fs.fs_lock);
return -ENOENT;


DELETE_FOUND:
struct ktfs_inode file_inode;
ret = ktfs_read_inode(target_inode, &file_inode);
if (ret < 0) {
    lock_release(&fs.fs_lock);
    return ret;
}


// Clear inode data
memset(&file_inode, 0, sizeof(struct ktfs_inode));


uint32_t inodes_per_block = KTFS_BLKSZ / KTFS_INOSZ;
uint32_t blk_num = 1 + fs.sb.bitmap_block_count + (target_inode / inodes_per_block);
uint32_t offset = (target_inode % inodes_per_block) * KTFS_INOSZ;


void* blkptr;
ret = cache_get_block(fs.cache, blk_num * KTFS_BLKSZ, &blkptr);
if (ret < 0) {
    lock_release(&fs.fs_lock);
    return ret;
}


memcpy((char*)blkptr + offset, &file_inode, sizeof(struct ktfs_inode));
cache_release_block(fs.cache, blkptr, 1);


ret = ktfs_read_data_block(root_inode.block[block_idx], dentries);
if (ret < 0) {
    lock_release(&fs.fs_lock);
    return ret;
}


for (int i = entry_idx; i < (KTFS_BLKSZ / KTFS_DENSZ) - 1; i++) {
    dentries[i] = dentries[i + 1];
}
memset(&dentries[(KTFS_BLKSZ / KTFS_DENSZ) - 1], 0, sizeof(struct ktfs_dir_entry));


uint32_t dentry_blk_idx = 1 + fs.sb.bitmap_block_count + fs.sb.inode_block_count + root_inode.block[block_idx];
ret = cache_get_block(fs.cache, dentry_blk_idx * KTFS_BLKSZ, &blkptr);
if (ret < 0) {
    lock_release(&fs.fs_lock);
    return ret;
}


memcpy(blkptr, dentries, KTFS_BLKSZ);
cache_release_block(fs.cache, blkptr, 1);


lock_release(&fs.fs_lock);
return 0;

}



