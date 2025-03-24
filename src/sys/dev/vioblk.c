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
}

static int vioblk_open(struct io ** ioptr, void * aux){

}

static long vioblk_readat (struct io * io, unsigned long long pos, void * buf, long bufsz){

}

static long vioblk_writeat (struct io * io, unsigned long long pos, const void * buf, long len){

}

static int vioblk_cntl (struct io * io, int cmd, void * arg){

}

static void vioblk_isr(int srcno, void * aux){

}