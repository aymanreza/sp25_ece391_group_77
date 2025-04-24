// viorng.c - VirtIO rng device with locking
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "virtio.h"
#include "intr.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "error.h"
#include "string.h"
#include "ioimpl.h"
#include "assert.h"
#include "conf.h"
#include "console.h"
#include "thread.h"

#include <stdint.h>

#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif

#ifndef VIORNG_NAME
#define VIORNG_NAME "rng"
#endif

#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif

struct viorng_device {
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
        struct virtq_desc desc[1];
    } vq;

    unsigned int bufcnt;
    char buf[VIORNG_BUFSZ];

    struct condition entropy_ready;
    struct lock lock;             // device-level mutex
};

static int viorng_open(struct io ** ioptr, void * aux);
static void viorng_close(struct io * io);
static long viorng_read(struct io * io, void * buf, long bufsz);
static void viorng_isr(int irqno, void * aux);

void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    static const struct iointf viorng_iointf = {
        .close = &viorng_close,
        .read  = &viorng_read
    };
    struct viorng_device *dev;
    virtio_featset_t enabled_features, wanted_features, needed_features;
    int result;
    uint16_t queue_size;

    assert(regs->device_id == VIRTIO_ID_RNG);

    regs->status |= VIRTIO_STAT_DRIVER;
    __sync_synchronize();

    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);
    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    regs->queue_sel = 0;
    __sync_synchronize();

    queue_size = regs->queue_num_max;
    if (queue_size == 0) {
        kprintf("viorng: Queue not available\n");
        return;
    }
    regs->queue_num = queue_size;
    __sync_synchronize();

    dev = kcalloc(1, sizeof(*dev));
    if (!dev) {
        kprintf("viorng: allocation failed\n");
        return;
    }
    dev->regs   = regs;
    dev->irqno  = irqno;
    dev->bufcnt = 0;

    ioinit0(&dev->io, &viorng_iointf);

    dev->vq.desc[0].addr  = (uint64_t)(uintptr_t)dev->buf;
    dev->vq.desc[0].len   = VIORNG_BUFSZ;
    dev->vq.desc[0].flags = VIRTQ_DESC_F_WRITE;

    virtio_attach_virtq(regs, 0, 1,
        (uint64_t)(uintptr_t)dev->vq.desc,
        (uint64_t)(uintptr_t)&dev->vq.used,
        (uint64_t)(uintptr_t)&dev->vq.avail);

    if (register_device(VIORNG_NAME, viorng_open, dev) != 0) {
        kprintf("viorng: Failed to register device\n");
        kfree(dev);
        return;
    }

    enable_intr_source(irqno, VIORNG_IRQ_PRIO, viorng_isr, dev);

    condition_init(&dev->entropy_ready, "viorng_ready");
    lock_init(&dev->lock);

    regs->status |= VIRTIO_STAT_DRIVER_OK;
    __sync_synchronize();
}

int viorng_open(struct io ** ioptr, void * aux) {
    struct viorng_device *dev = aux;
    if (!dev)
        return -ENODEV;

    lock_acquire(&dev->lock);
    dev->vq.avail.idx      = 0;
    dev->vq.used.idx       = 0;
    dev->vq.last_used_idx  = 0;

    virtio_enable_virtq(dev->regs, 0);
    enable_intr_source(dev->irqno, VIORNG_IRQ_PRIO, viorng_isr, dev);
    dev->io.refcnt++;
    lock_release(&dev->lock);

    *ioptr = &dev->io;
    return 0;
}

void viorng_close(struct io * io) {
    struct viorng_device *dev =
        (void*)io - offsetof(struct viorng_device, io);
    if (!dev)
        return;

    lock_acquire(&dev->lock);
    dev->vq.avail.idx      = 0;
    dev->vq.used.idx       = 0;
    dev->vq.last_used_idx  = 0;

    disable_intr_source(dev->irqno);
    if (dev->io.refcnt > 0)
        dev->io.refcnt--;
    lock_release(&dev->lock);
}

long viorng_read(struct io * io, void * buf, long bufsz) {
    struct viorng_device *dev =
        (void*)io - offsetof(struct viorng_device, io);
    char *output_buffer = buf;
    long byte_count = 0;
    long size;

    if (bufsz == 0)
        return 0;

    lock_acquire(&dev->lock);

    // request randomness
    dev->vq.desc[0].addr  = (uint64_t)(uintptr_t)dev->buf;
    dev->vq.desc[0].len   = VIORNG_BUFSZ;
    dev->vq.desc[0].flags = VIRTQ_DESC_F_WRITE;

    dev->vq.avail.ring[dev->vq.avail.idx % 1] = 0;
    __sync_synchronize();
    dev->vq.avail.idx++;
    __sync_synchronize();
    virtio_notify_avail(dev->regs, 0);

    while (dev->bufcnt == 0) {
        condition_wait(&dev->entropy_ready);
    }

    while (byte_count < bufsz && byte_count < VIORNG_BUFSZ && dev->bufcnt > 0) {
        size = (dev->bufcnt < (bufsz - byte_count)) ? dev->bufcnt : (bufsz - byte_count);
        memcpy(output_buffer + byte_count, dev->buf + (VIORNG_BUFSZ - dev->bufcnt), size);
        dev->bufcnt -= size;
        byte_count += size;
    }

    lock_release(&dev->lock);
    return byte_count;
}

void viorng_isr(int irqno, void * aux) {
    struct viorng_device *dev = aux;
    uint32_t status = dev->regs->interrupt_status;
    dev->regs->interrupt_ack = status;

    if (status & 0x1) {
        lock_acquire(&dev->lock);
        if (dev->vq.used.idx != dev->vq.last_used_idx) {
            dev->bufcnt = dev->vq.used.ring[0].len;
            condition_broadcast(&dev->entropy_ready);
        }
        dev->vq.last_used_idx = dev->vq.used.idx;
        lock_release(&dev->lock);
    }
}
