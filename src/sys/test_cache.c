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
#include "cache.h"

#include <string.h>

#define VIRTIO_MMIO_STEP (VIRTIO1_MMIO_BASE-VIRTIO0_MMIO_BASE)
extern char _kimg_end[]; 
int basic_test(void) {
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





}


void main(void) {
    int pass = basic_test();
    assert(pass);
}
