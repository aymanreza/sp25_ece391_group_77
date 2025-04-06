#include "conf.h"
#include "heap.h"
#include "console.h"
#include "elf.h"
#include "assert.h"
#include "thread.h"
#include "fs.h"
#include "io.h"
#include "device.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "intr.h"
#include "dev/virtio.h"

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)
extern char _kimg_end[]; 
void main(void) {
    struct io *blkio;
    struct io *termio;
    struct io *trekio;
    int result;
    int i;
    int tid;
    void (*exe_entry)(struct io*);

    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    heap_init(_kimg_end, UMEM_START); 
    uart_attach((void*)UART0_MMIO_BASE, UART0_INTR_SRCNO+0);
    uart_attach((void*)UART1_MMIO_BASE, UART0_INTR_SRCNO+1);
    rtc_attach((void*)RTC_MMIO_BASE);
    
    for (i = 0; i < 8; i++) {
        virtio_attach ((void*)VIRTIO0_MMIO_BASE + i*VIRTIO_MMIO_STEP, VIRTIO0_INTR_SRCNO + i);
    }

    result = open_device("vioblk", 0, &blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open vioblk\n");
    }

    result = fsmount(blkio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to mount filesystem\n");
    }

    result = open_device("uart", 1, &termio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open UART\n");
    }

    result = fsopen("trek", &trekio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open trek\n");
    }

    // TODO:
    // 1. Load the trek file into memory
    result = elf_load(trekio, &exe_entry);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to load trek file\n");
    }
    // 2. Verify the loading of the file into memory
    kprintf("Trek loaded at entry point 0x%lx\n", (unsigned long)exe_entry);
    trek_start = exe_entry; // setting global entry for call to thrfn
    // 3. Run trek on a new thread
    tid = thread_spawn("trek", trek_thrfn);
    if (tid < 0) {
         kprintf("Error: %d\n", tid);
         panic("Failed to spawn trek thread\n");
    }
    kprintf("Trek thread spawned successfully\n");
    // 4. Verify that the thread was able to run properly, if it was have the main thread wait for trek to finish
    result = thread_join(tid);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Error joining trek thread\n");
    }
    kprintf("Trek thread finished successfully.\n");
}

static void (*trek_start)(struct io*) = NULL;

static void trek_thrfn(void) {
    struct io *termio;
    int result;

    result = open_device("uart", 1, &termio);
    assert(result == 0);
    trek_entry(termio);
}