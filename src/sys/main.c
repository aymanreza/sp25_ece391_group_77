#include "conf.h"
#include "console.h"
#include "elf.h"
#include "assert.h"
#include "thread.h"
#include "process.h"
#include "memory.h"
#include "fs.h"
#include "io.h"
#include "device.h"
#include "dev/rtc.h"
#include "dev/uart.h"
#include "intr.h"
#include "dev/virtio.h"
#include "heap.h"
#include "string.h"

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)
extern char _kimg_end[]; 




void main(void) {
    struct io *blkio;
    int result;
    int i;

    
    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    memory_init();
    procmgr_init();


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

    result = open_device("uart", 1, &current_process()->iotab[2]);
    if (result < 0) {
        kprintf("Error: %d\n", result);
        panic("Failed to open uart");
    }

    // insert testcase below

    // // Launch trek_cp2
    // struct io *trekio;
    // result = fsopen("trek_cp2", &trekio);
    // if (result < 0) panic("Failed to open trek_cp2");
    // result = process_exec(trekio, 0, NULL);
    // assert(result == 0);
    // thread_join(0);

    // Launch zork 
    struct io *zorkio;
    result = fsopen("zork", &zorkio);
    if (result < 0) panic("Failed to open zork");
    result = process_exec(zorkio, 0, NULL); 
    assert(result == 0);
    thread_join(0);

    // Launch rogue with filename "roguesave.dat" as argv[1]
    // struct io *rogueio;
    // result = fsopen("rogue", &rogueio);
    // if (result < 0) panic("Failed to open rogue");
    // // char *rogue_argv[3];
    // // // rogue_argv[0] = (char *)"rogue";
    // // // rogue_argv[1] = (char *)"roguesave.dat";
    // // // rogue_argv[2] = NULL;
    // result = process_exec(rogueio, 0, NULL);
    // assert(result == 0);
    // thread_join(0);
    
}
