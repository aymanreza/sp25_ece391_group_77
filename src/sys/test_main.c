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

#include <string.h>

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)
extern char _kimg_end[]; 
void main(void) {
    console_init();
    devmgr_init();
    intrmgr_init();
    thrmgr_init();
    heap_init(_kimg_end, UMEM_START); 
    uart_attach((void*)UART0_MMIO_BASE, UART0_INTR_SRCNO+0);
    uart_attach((void*)UART1_MMIO_BASE, UART0_INTR_SRCNO+1);
    rtc_attach((void*)RTC_MMIO_BASE);

    for (int i = 0; i < 8; i++) {
        virtio_attach((void*)(VIRTIO0_MMIO_BASE + i * VIRTIO_MMIO_STEP), VIRTIO0_INTR_SRCNO + i);
    }

    // Attach UART
    uart_attach((void*)UART0_MMIO_BASE, UART0_INTR_SRCNO);

    struct io *blkio;
    int result = open_device("vioblk", 0, &blkio);
    assert(result == 0);
    kprintf("[ok] vioblk device opened\n");

    // Creating a test buffer
    char write_buf[512] = "Hello ECE391!";
    char read_buf[512] = {0};

    // Write to block 0
    result = iowriteat(blkio, 0, write_buf, 512);
    assert(result == 512);
    kprintf("[ok] vioblk write to block 0\n");

    // Read back block 0
    result = ioreadat(blkio, 0, read_buf, 512);
    assert(result == 512);
    assert(strcmp(write_buf, read_buf) == 0);
    kprintf("[ok] vioblk read matches write\n");

    ioclose(blkio);
    kprintf("=== vioblk test passed!!! ===\n");

    kprintf("System halted.\n");
    asm volatile("wfi"); // Wait For Interrupt (RISC-V sleep)
}
