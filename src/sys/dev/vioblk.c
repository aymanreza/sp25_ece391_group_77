// vioblk.c - VirtIO serial port (console)
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include "virtio.h"
#include "intr.h"
#include "assert.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "thread.h"
#include "error.h"
#include "string.h"
#include "assert.h"
#include "ioimpl.h"
#include "io.h"
#include "conf.h"

#include <limits.h>

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif

// INTERNAL CONSTANT DEFINITIONS
//

// VirtIO block device feature bits (number, *not* mask)

#define VIRTIO_BLK_F_SIZE_MAX       1
#define VIRTIO_BLK_F_SEG_MAX        2
#define VIRTIO_BLK_F_GEOMETRY       4
#define VIRTIO_BLK_F_RO             5
#define VIRTIO_BLK_F_BLK_SIZE       6
#define VIRTIO_BLK_F_FLUSH          9
#define VIRTIO_BLK_F_TOPOLOGY       10
#define VIRTIO_BLK_F_CONFIG_WCE     11
#define VIRTIO_BLK_F_MQ             12
#define VIRTIO_BLK_F_DISCARD        13
#define VIRTIO_BLK_F_WRITE_ZEROES   14

// Request types

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_T_FLUSH 4
#define VIRTIO_BLK_T_GET_ID 8
#define VIRTIO_BLK_T_GET_LIFETIME 10
#define VIRTIO_BLK_T_DISCARD 11
#define VIRTIO_BLK_T_WRITE_ZEROES 13
#define VIRTIO_BLK_T_SECURE_ERASE 14

#define GETBLKSZ  0 //?????
#define GETEND    2

// INTERNAL TYPE DEFINITIONS
//

// ADDED THIS VIOBLK DEVICE STRUCT MYSELF
struct vioblk_device {
    volatile struct virtio_mmio_regs * regs;
    int irqno;
    int instno;
    struct io io;

    struct {
        uint16_t last_used_idx;

        union {
            struct virtq_avail avail;
            char _avail_filler[VIRTQ_AVAIL_SIZE(1)];
        };

        union {
            volatile struct virtq_used used;
            char _used_filler[VIRTQ_USED_SIZE(1)];
        };

        // The first descriptor is a regular descriptor and is the one used in
        // the avail and used rings.

        struct virtq_desc desc[3];
    } vq;

    uint32_t blksz;   // Block size 
    struct condition data_cond; // for threads
};

struct virtio_blk_req {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

// INTERNAL FUNCTION DECLARATIONS
//

static int vioblk_open(struct io ** ioptr, void * aux);
static void vioblk_close(struct io * io);

static long vioblk_readat (
    struct io * io,
    unsigned long long pos,
    void * buf,
    long bufsz);

static long vioblk_writeat (
    struct io * io,
    unsigned long long pos,
    const void * buf,
    long len);

static int vioblk_cntl (
    struct io * io, int cmd, void * arg);

static void vioblk_isr(int srcno, void * aux);

// EXPORTED FUNCTION DEFINITIONS
//

// Attaches a VirtIO block device. Declared and called directly from virtio.c.

void vioblk_attach(volatile struct virtio_mmio_regs * regs, int irqno) {

    // added missing variable declarations?
    virtio_featset_t needed_features;
    virtio_featset_t wanted_features;
    virtio_featset_t enabled_features;
    int result;
    uint32_t blksz;

    assert(regs->device_id == VIRTIO_ID_BLOCK); // making sure id is correct

    // Negotiate features. We need:
    //  - VIRTIO_F_RING_RESET and
    //  - VIRTIO_F_INDIRECT_DESC
    // We want:
    //  - VIRTIO_BLK_F_BLK_SIZE and
    //  - VIRTIO_BLK_F_TOPOLOGY.

    virtio_featset_init(needed_features);
    virtio_featset_add(needed_features, VIRTIO_F_RING_RESET);
    virtio_featset_add(needed_features, VIRTIO_F_INDIRECT_DESC);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_TOPOLOGY);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);

    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    // If the device provides a block size, use it. Otherwise, use 512.

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        blksz = regs->config.blk.blk_size;
    else
        blksz = 512;

    // blksz must be a power of two
    assert (((blksz - 1) & blksz) == 0);

    // FIX ME

    struct vioblk_device * dev = kcalloc(1, sizeof(*dev)); // creating a new block device
    dev->regs = regs;
    dev->irqno = irqno;
    dev->blksz = blksz;

    // define the I/O ops for this device
    static const struct iointf blk_iointf = {
        .close = &vioblk_close,    // vioblk_close
        .readat  = &vioblk_readat,     // vioblk_readat
        .writeat = &vioblk_writeat, // vioblk_writeat
        .cntl = &vioblk_cntl, // vioblk_cntl
    };
    dev->io.intf = &blk_iointf;
    
    // set up virtqueue 
    memset(&dev->vq, 0, sizeof(dev->vq));  // zeroing out the virtqueue struct

    virtio_attach_virtq(regs, 
        0, // 0 is the qid for virtioblk
        3, // 3 descriptors
        (uint64_t)(uintptr_t)(&dev->vq.desc[0]), // address of first descriptor
        (uint64_t)(uintptr_t)(&dev->vq.used), // address of used ring
        (uint64_t)(uintptr_t)(&dev->vq.avail) // address of available ring
    );

    virtio_enable_virtq(regs, 0); // enabling virtqueue for this device
    __sync_synchronize();
    

    //register the ISR
    enable_intr_source(dev->irqno, VIOBLK_INTR_PRIO, vioblk_isr, dev);

    // Mark the driver as ready 
    regs->status |= VIRTIO_STAT_DRIVER_OK;
    __sync_synchronize();

    //register the device
    dev->instno = register_device(VIOBLK_NAME, vioblk_open, dev);

    condition_init(&dev->data_cond, "viorng_data_cond"); //initializing data condition
}

static int vioblk_open(struct io ** ioptr, void * aux) {

    if (!ioptr || !aux)
        return -EINVAL;

    // retreiving device from aux argument
    struct vioblk_device * dev = (struct vioblk_device *)aux;

    // // reinitialize virtqueue indicies so they are 'available' for use
    // dev->vq.avail.idx       = 0;
    // dev->vq.used.idx        = 0;
    // dev->vq.last_used_idx   = 0;

    // bump refcount and return the io pointer
    ioaddref(&dev->io);  
    *ioptr = &dev->io;
    return 0;
}

static void vioblk_close(struct io * io) {
    // validating io argument
    assert (io != NULL);
    assert (io->intf != NULL);

    // reset virtqueues
    struct vioblk_device * dev = (struct vioblk_device *)((char*)io - offsetof(struct vioblk_device, io)); //retreiving virtio device from io pointer

    // resetting the virtqueue
    virtio_reset_virtq(dev->regs, 0);

    // disabling interrupts for this source
    disable_intr_source(dev->irqno);

    // io refcount is taken care of in the ioclose function (io.c)

    debug("Device successfully closed in vioblk_close \n");
}

static long vioblk_readat (struct io * io, unsigned long long pos, void * buf, long bufsz) {
    // checking validity of the arguments
    assert(io != NULL && buf != NULL && bufsz > 0);

    // getting the vioblk device using io pointer
    struct vioblk_device * dev = (struct vioblk_device *)((char*)io - offsetof(struct vioblk_device, io)); //retreiving virtio device

    // ensuring 512 byte alignment
    if (pos % dev->blksz != 0 || bufsz % dev->blksz != 0)
    return -EINVAL;

    // creating request for the virtqueue
    struct virtio_blk_req req = {
        .type = VIRTIO_BLK_T_IN,
        .reserved = 0,
        .sector = pos / dev->blksz
    };

    uint8_t status = 0;

    // filling in descriptor #1, the request header (16 bytes)
    dev->vq.desc[0].addr = (uint64_t)(uintptr_t)&req;
    dev->vq.desc[0].flags = VIRTQ_DESC_F_NEXT;
    dev->vq.desc[0].len =  sizeof(req);
    dev->vq.desc[0].next = 1; // next is desc[1]

    // filling in descriptor #2, the data buffer (512 bytes)
    dev->vq.desc[1].addr = (uint64_t)(uintptr_t)buf;
    dev->vq.desc[1].flags = VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT;
    dev->vq.desc[1].len = bufsz;
    dev->vq.desc[1].next = 2;

    // filling in descriptor #3, the status (1 byte)
    dev->vq.desc[2].addr = (uint64_t)(uintptr_t)&status;
    dev->vq.desc[2].flags = VIRTQ_DESC_F_WRITE;
    dev->vq.desc[2].len = 1;
    dev->vq.desc[2].next = -1;
  
    // submitting the descriptor chain
    uint16_t avail_idx = dev->vq.avail.idx % 3;
    dev->vq.avail.ring[avail_idx] = 0; // descriptor chain starts at index 0
    __sync_synchronize();
    dev->vq.avail.idx++;
    __sync_synchronize();

    // notify the device
    virtio_notify_avail(dev->regs, 0);

    // sleep until used ring shows the descriptor is completed (will be updated by device)
    int pie = disable_interrupts();
    while (dev->vq.last_used_idx == dev->vq.used.idx) {
        restore_interrupts(pie);

        // wait for thread wake
        condition_wait(&dev->data_cond);

        pie = disable_interrupts();
    }
    restore_interrupts(pie);
    __sync_synchronize();

    dev->vq.last_used_idx++;

    // check the status byte in descriptor #2
    if (status != 0)
        return -EIO;

    memset(&dev->vq.desc[0], 0, sizeof(dev->vq.desc[0]) * 3); //zeroing out descriptors after one request

    return bufsz;
}

static long vioblk_writeat (struct io * io, unsigned long long pos, const void * buf, long len) {
    // checking validity of the arguments
    assert(io != NULL && buf != NULL && len > 0);

    // getting the vioblk device using io pointer
    struct vioblk_device * dev = (struct vioblk_device *)((char*)io - offsetof(struct vioblk_device, io)); //retreiving virtio device

    // ensuring 512 byte alignment
    if (pos % dev->blksz != 0 || len % dev->blksz != 0)
    return -EINVAL;

    // creating request for the virtqueue
    struct virtio_blk_req req = {
        .type = VIRTIO_BLK_T_OUT,
        .reserved = 0,
        .sector = pos / dev->blksz
    };

    uint8_t status = 0; // initilizing the status byte for the descriptor

    // filling in descriptor #1, the request header (16 bytes)
    dev->vq.desc[0].addr = (uint64_t)(uintptr_t)&req;
    dev->vq.desc[0].flags = VIRTQ_DESC_F_NEXT;
    dev->vq.desc[0].len =  sizeof(req);
    dev->vq.desc[0].next = 1; // next is desc[1]

    // filling in descriptor #2, the data buffer (512 bytes)
    dev->vq.desc[1].addr = (uint64_t)(uintptr_t)buf;
    dev->vq.desc[1].flags = VIRTQ_DESC_F_NEXT;
    dev->vq.desc[1].len = len;
    dev->vq.desc[1].next = 2;

    // filling in descriptor #3, the status (1 byte)
    dev->vq.desc[2].addr = (uint64_t)(uintptr_t)&status;
    dev->vq.desc[2].flags = VIRTQ_DESC_F_WRITE;
    dev->vq.desc[2].len = 1;
    dev->vq.desc[2].next = -1;

    // submitting the descriptor chain
    uint16_t avail_idx = dev->vq.avail.idx % 3;
    dev->vq.avail.ring[avail_idx] = 0; // descriptor chain starts at index 0
    __sync_synchronize();
    dev->vq.avail.idx++;
    __sync_synchronize();

    // notify the device
    virtio_notify_avail(dev->regs, 0);

    // sleep until used ring shows the descriptor is completed (will be updated by device)
    int pie = disable_interrupts();
    while (dev->vq.last_used_idx == dev->vq.used.idx) {
        restore_interrupts(pie);

            // wait for thread wake
            condition_wait(&dev->data_cond);

        pie = disable_interrupts();
    }
    restore_interrupts(pie);
    __sync_synchronize();

    dev->vq.last_used_idx++;

    // check the status byte in descriptor #2
    if (status != 0)
        return -EIO;

    memset(&dev->vq.desc[0], 0, sizeof(dev->vq.desc[0]) * 3); //zeroing out descriptors after one request

    return len;
}

static int vioblk_cntl (struct io * io, int cmd, void * arg) {
    
    // getting the vioblk device using io pointer
    struct vioblk_device * dev = (struct vioblk_device *)((char*)io - offsetof(struct vioblk_device, io)); //retreiving virtio device

    switch(cmd) { 
        // GETBLKSZ is our command
        case GETBLKSZ: {
            if(!arg) // if there is no argument, return error invalid
                return -EINVAL;
            *(unsigned long *)arg = dev->blksz; //return blocksize using arg argument
           return 0;
        }

        // GETEND is out command
        case GETEND: {
            if(!arg) // if there is no argument, return error invalid
                return -EINVAL;
           *(unsigned long long *)arg = dev->regs->config.blk.capacity * dev->blksz;
            return 0;
        }

        // command is not supported, error
        default:
            return -ENOTSUP;
    }
}

static void vioblk_isr(int srcno, void * aux) {
    // retreiving the vioblk device
    struct vioblk_device *dev = (struct vioblk_device *)aux; 

    // read interrupt status
    uint32_t isr_status = dev->regs->interrupt_status;
    if (isr_status == 0) {
        // no interrupt to acknowledge
        return;
    }
    // acknowledge the interrupt at the device level
    dev->regs->interrupt_ack = isr_status;

    // process completed requests (advance used index)
    int pie = disable_interrupts();
    while (dev->vq.last_used_idx != dev->vq.used.idx) {
        dev->vq.last_used_idx++;
    }
    restore_interrupts(pie);

    // wake up the thread waiting on the request
    condition_broadcast(&dev->data_cond);
}