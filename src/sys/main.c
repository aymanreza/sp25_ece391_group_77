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
struct io * global_termio;


static void trek_thrfn(void) {
    void (*entry)(struct io *);
    struct io *trekio;
    int result;

    // Open the trek file again inside the thread
    result = fsopen("trek", &trekio);
    assert(result == 0);

    // Load the ELF and get entry point
    result = elf_load(trekio, (void (**)(void)) &entry);
    assert(result == 0);

    // Call the user-level entry with the terminal IO
    entry(global_termio);
}


void main(void) {
    struct io *blkio;
    struct io *termio;
    struct io *trekio;
    int result;
    int i;
    int tid;
    // void (*exe_entry)(struct io*);

    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    heap_init(_kimg_end, UMEM_START); 
    uart_attach((void*)UART0_MMIO_BASE, UART0_INTR_SRCNO+0);
    uart_attach((void*)UART1_MMIO_BASE, UART0_INTR_SRCNO+1);
    rtc_attach((void*)RTC_MMIO_BASE);

    // int trektid;
    
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
    // 2. Verify the loading of the file into memory
    global_termio = termio;

    // 3. Run trek on a new thread
    tid = thread_spawn("trek", trek_thrfn);

    // 4. Verify that the thread was able to run properly, if it was have the main thread wait for trek to finish
    // assert (0 < trektid);
    
    assert(tid > 0);
    thread_join(0);

}

