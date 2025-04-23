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
#include "memory.h"
#include "process.h"

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)
extern char _kimg_end[]; 
void main(void) {
    struct io *blkio;
    struct io *termio;
    struct io *trekio;
    int result;
    int i;
    // int tid;
    // void (*exe_entry)(void);

    console_init();
    memory_init(); // added memory initialization
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    procmgr_init(); // added process manager initialization
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

    result = fsopen("trek_cp2", &trekio);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open trek\n");
    }

    // // TODO:
    // // 1. Load the trek file into memory
    // result = elf_load(trekio, &exe_entry);
    // // 2. Verify the loading of the file into memory
    // assert(result == 0);
    // // 3. Run trek on a new thread
    // int tid = thread_spawn("trek_cp2", exe_entry, termio);
    
    // // 4. Verify that the thread was able to run properly, if it was have the main thread wait for trek to finish
    // assert(tid > 0);
    // thread_set_process(tid, thread_process(tid));
    // thread_join(0);

    //Flexio said on discord: quick thing, trek takes in a termio as an argument just to help whoever is struggling with running trek??????
    void *argv[1];
    argv[0] = termio;
    result = process_exec(trekio, 1, (char **)argv);
    assert(result == 0);
    thread_join(0);  // wait for process 0 (main thread) to exit
}