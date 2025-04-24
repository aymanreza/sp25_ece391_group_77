// io.c - Unified I/O object
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "io.h"
#include "ioimpl.h"
#include "assert.h"
#include "string.h"
#include "heap.h"
#include "error.h"
#include "thread.h"
#include "memory.h"

#include <stddef.h>
#include <limits.h>
// INTERNAL TYPE DEFINITIONS
//

struct memio {
    struct io io; // I/O struct of memory I/O
    void * buf; // Block of memory
    size_t size; // Size of memory block
};

// #define PIPE_BUFSZ PAGE_SIZE

struct seekio {
    struct io io; // I/O struct of seek I/O
    struct io * bkgio; // Backing I/O supporting _readat_ and _writeat_
    unsigned long long pos; // Current position within backing endpoint
    unsigned long long end; // End position in backing endpoint
    int blksz; // Block size of backing endpoint
};

// INTERNAL FUNCTION DEFINITIONS
//

static int memio_cntl(struct io * io, int cmd, void * arg);

static long memio_readat (
    struct io * io, unsigned long long pos, void * buf, long bufsz);

static long memio_writeat (
    struct io * io, unsigned long long pos, const void * buf, long len);

static void seekio_close(struct io * io);

static int seekio_cntl(struct io * io, int cmd, void * arg);

static long seekio_read(struct io * io, void * buf, long bufsz);

static long seekio_write(struct io * io, const void * buf, long len);

static long seekio_readat (
    struct io * io, unsigned long long pos, void * buf, long bufsz);

static long seekio_writeat (
    struct io * io, unsigned long long pos, const void * buf, long len);


// INTERNAL GLOBAL CONSTANTS
static const struct iointf seekio_iointf = {
    .close = &seekio_close,
    .cntl = &seekio_cntl,
    .read = &seekio_read,
    .write = &seekio_write,
    .readat = &seekio_readat,
    .writeat = &seekio_writeat
};

// setting up memio io interface
static const struct iointf memio_iointf = {
    .cntl = &memio_cntl,
    .readat = &memio_readat,
    .writeat = &memio_writeat
};

// EXPORTED FUNCTION DEFINITIONS
//

struct io * ioinit0(struct io * io, const struct iointf * intf) {
    assert (io != NULL);
    assert (intf != NULL);
    io->intf = intf;
    io->refcnt = 0;
    return io;
}

struct io * ioinit1(struct io * io, const struct iointf * intf) {
    assert (io != NULL);
    io->intf = intf;
    io->refcnt = 1;
    return io;
}

unsigned long iorefcnt(const struct io * io) {
    assert (io != NULL);
    return io->refcnt;
}

struct io * ioaddref(struct io * io) {
    assert (io != NULL);
    io->refcnt += 1;
    return io;
}

void ioclose(struct io * io) {
    assert (io != NULL);
    assert (io->intf != NULL);
    
    assert (io->refcnt != 0);
    io->refcnt -= 1;

    if (io->refcnt == 0 && io->intf->close != NULL)
        io->intf->close(io);
}

long ioread(struct io * io, void * buf, long bufsz) {
    assert (io != NULL);
    assert (io->intf != NULL);

    if (io->intf->read == NULL)
        return -ENOTSUP;
    
    if (bufsz < 0)
        return -EINVAL;
    
    return io->intf->read(io, buf, bufsz);
}

long iofill(struct io * io, void * buf, long bufsz) {
	long bufpos = 0; // position in buffer for next read
    long nread; // result of last read

    assert (io != NULL);
    assert (io->intf != NULL);

    if (io->intf->read == NULL)
        return -ENOTSUP;

    if (bufsz < 0)
        return -EINVAL;

    while (bufpos < bufsz) {
        nread = io->intf->read(io, buf+bufpos, bufsz-bufpos);
        
        if (nread <= 0)
            return (nread < 0) ? nread : bufpos;
        
        bufpos += nread;
    }

    return bufpos;
}

long iowrite(struct io * io, const void * buf, long len) {
	long bufpos = 0; // position in buffer for next write
    long n; // result of last write

    assert (io != NULL);
    assert (io->intf != NULL);
    
    if (io->intf->write == NULL)
        return -ENOTSUP;

    if (len < 0)
        return -EINVAL;
    
    do {
        n = io->intf->write(io, buf+bufpos, len-bufpos);

        if (n <= 0)
            return (n < 0) ? n : bufpos;

        bufpos += n;
    } while (bufpos < len);

    return bufpos;
}

long ioreadat (
    struct io * io, unsigned long long pos, void * buf, long bufsz)
{
    assert (io != NULL);
    assert (io->intf != NULL);
    
    if (io->intf->readat == NULL)
        return -ENOTSUP;
    
    if (bufsz < 0)
        return -EINVAL;
    
    return io->intf->readat(io, pos, buf, bufsz);
}

long iowriteat (
    struct io * io, unsigned long long pos, const void * buf, long len)
{
    assert (io != NULL);
    assert (io->intf != NULL);
    
    if (io->intf->writeat == NULL)
        return -ENOTSUP;
    
    if (len < 0)
        return -EINVAL;
    
    return io->intf->writeat(io, pos, buf, len);
}

int ioctl(struct io * io, int cmd, void * arg) {
    assert (io != NULL);
    assert (io->intf != NULL);

	if (io->intf->cntl != NULL)
        return io->intf->cntl(io, cmd, arg);
    else if (cmd == IOCTL_GETBLKSZ)
        return 1; // default block size
    else
        return -ENOTSUP;
}

int ioblksz(struct io * io) {
    return ioctl(io, IOCTL_GETBLKSZ, NULL);
}

int ioseek(struct io * io, unsigned long long pos) {
    return ioctl(io, IOCTL_SETPOS, &pos);
}

struct io * create_memory_io(void * buf, size_t size) {
    // FIX ME
    // checking for valid inputs

    assert(buf && size > 0);

    // allocating memory for memio struct
    struct memio *mio = kcalloc(1, sizeof(struct memio));
    
    assert(mio);

    // initializing memio struct paramenters
    mio->buf = buf;
    mio->size = size;

    // initializing memio io
    return ioinit1(&mio->io, &memio_iointf);
}

struct io * create_seekable_io(struct io * io) {
    struct seekio * sio;
    unsigned long end;
    int result;
    int blksz;
    
    blksz = ioblksz(io);
    assert (0 < blksz);
    
    // block size must be power of two
    assert ((blksz & (blksz - 1)) == 0);

    result = ioctl(io, IOCTL_GETEND, &end);
    assert (result == 0);
    
    sio = kcalloc(1, sizeof(struct seekio));

    sio->pos = 0;
    sio->end = end;
    sio->blksz = blksz;
    sio->bkgio = ioaddref(io);

    return ioinit1(&sio->io, &seekio_iointf);

};

// INTERNAL FUNCTION DEFINITIONS
//

long memio_readat (
    struct io * io,
    unsigned long long pos,
    void * buf, long bufsz)
{
    // FIX ME
    // making sure io and interface are valid
    assert(io != NULL);
    assert(io->intf != NULL);

    // making sure bufsz is not negative
    if (bufsz < 0) {
        return -EINVAL;
    }

    struct memio * const mio = (void*)io - offsetof(struct memio, io);

    // if position is greater than or equal to memio size, then there is nothing to read
    if (pos >= mio->size) {
        return 0;
    }

    // clamping bufsz to remaining buffer space if it is too large
    if ((unsigned long long)bufsz > mio->size - pos) {
        bufsz = mio->size - pos;
    }

    // copying memory from memio to provided buffer
    memcpy(buf, mio->buf + pos, bufsz);

    return bufsz;
}

long memio_writeat (
    struct io * io,
    unsigned long long pos,
    const void * buf, long len)
{
    // FIX ME
    // making sure io and interface are valid
    assert(io != NULL);
    assert(io->intf != NULL);

    // making sure len is not negative
    if (len < 0) {
        return -EINVAL;
    }

    struct memio * const mio = (void*)io - offsetof(struct memio, io);

    // if position is greater than or equal to memio size, the value of pos is invalid
    if (pos >= mio->size) {
        return -EINVAL;
    }

    // clamping bufsz to remaining buffer space if it is too large
    if ((unsigned long long)len > mio->size - pos) {
        len = mio->size - pos;
    }

    // copying memory from provided buffer to memio
    memcpy(mio->buf + pos, buf, len);

    return len;
}

int memio_cntl(struct io * io, int cmd, void * arg) {
    // FIX ME
    // making sure io and interface are valid
    assert(io != NULL);
    assert(io->intf != NULL);

    struct memio * const mio = (void*)io - offsetof(struct memio, io);
    unsigned long long * ullarg = arg;

    switch (cmd) {
        // returning 1 in case of get block size command
        case IOCTL_GETBLKSZ:
            return 1;
        // storing total buffer size in arg and returning 0 for success
        case IOCTL_GETEND:
            *ullarg = mio->size;
            return 0;
        // returns error if new end exceeds buffer size; else, return 0 for success
        case IOCTL_SETEND:
            if (*ullarg > mio->size) {
                return -EINVAL;
            }
            return 0;
        // returns unsupported command error if not one of the three above
        default:
            return -ENOTSUP;
    }
}

void seekio_close(struct io * io) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    sio->bkgio->refcnt--;
    ioclose(sio->bkgio);
    kfree(sio);
}

int seekio_cntl(struct io * io, int cmd, void * arg) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    unsigned long long * ullarg = arg;
    int result;

    switch (cmd) {
    case IOCTL_GETBLKSZ:
        return sio->blksz;
    case IOCTL_GETPOS:
        *ullarg = sio->pos;
        return 0;
    case IOCTL_SETPOS:
        // New position must be multiple of block size
        if ((*ullarg & (sio->blksz - 1)) != 0)
            return -EINVAL;
        
        // New position must not be past end
        if (*ullarg > sio->end)
            return -EINVAL;
        
        sio->pos = *ullarg;
        return 0;
    case IOCTL_GETEND:
        *ullarg = sio->end;
        return 0;
    case IOCTL_SETEND:
        // Call backing endpoint ioctl and save result
        result = ioctl(sio->bkgio, IOCTL_SETEND, ullarg);
        if (result == 0)
            sio->end = *ullarg;
        return result;
    default:
        return ioctl(sio->bkgio, cmd, arg);
    }
}

long seekio_read(struct io * io, void * buf, long bufsz) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    unsigned long long const pos = sio->pos;
    unsigned long long const end = sio->end;
    long rcnt;

    // Cannot read past end
    if (end - pos < bufsz)
        bufsz = end - pos;

    if (bufsz == 0)
        return 0;
        
    // Request must be for at least blksz bytes if not zero
    if (bufsz < sio->blksz)
        return -EINVAL;

    // Truncate buffer size to multiple of blksz
    bufsz &= ~(sio->blksz - 1);

    rcnt = ioreadat(sio->bkgio, pos, buf, bufsz);
    sio->pos = pos + ((rcnt < 0) ? 0 : rcnt);
    return rcnt;
}


long seekio_write(struct io * io, const void * buf, long len) {
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    unsigned long long const pos = sio->pos;
    unsigned long long end = sio->end;
    int result;
    long wcnt;

    if (len == 0)
        return 0;
    
    // Request must be for at least blksz bytes
    if (len < sio->blksz)
        return -EINVAL;
    
    // Truncate length to multiple of blksz
    len &= ~(sio->blksz - 1);

    // Check if write is past end. If it is, we need to change end position.

    if (end - pos < len) {
        if (ULLONG_MAX - pos < len)
            return -EINVAL;
        
        end = pos + len;

        result = ioctl(sio->bkgio, IOCTL_SETEND, &end);
        
        if (result != 0)
            return result;
        
        sio->end = end;
    }

    wcnt = iowriteat(sio->bkgio, sio->pos, buf, len);
    sio->pos = pos + ((wcnt < 0) ? 0 : wcnt);
    return wcnt;
}

long seekio_readat (
    struct io * io, unsigned long long pos, void * buf, long bufsz)
{
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    return ioreadat(sio->bkgio, pos, buf, bufsz);
}

long seekio_writeat (
    struct io * io, unsigned long long pos, const void * buf, long len)
{
    struct seekio * const sio = (void*)io - offsetof(struct seekio, io);
    return iowriteat(sio->bkgio, pos, buf, len);
}