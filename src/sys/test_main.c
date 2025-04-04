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

#define GETBLKSZ  0 //?????
#define GETEND    2

void dump_buffer(const char *label, const void *buf, size_t len) {
    const unsigned char *data = buf;
    kprintf("\n--- %s (len = %zu) ---\n", label, len);
    for (size_t i = 0; i < len; i += 16) {
        kprintf("%04zx: ", i);
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            kprintf("%02x ", data[i + j]);
        }
        kprintf(" | ");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            char c = data[i + j];
            kprintf("%c", (c >= 32 && c <= 126) ? c : '.');
        }
        kprintf("\n");
    }
}

void basic_test_thread(void) {
    struct io *blkio;
    int result = open_device("vioblk", 0, &blkio);
    assert(result == 0);
    kprintf("\n✅ vioblk device opened\n");

    // Creating a test buffer
    char write_buf[512] = "Hello ECE391!";
    char read_buf[512] = {0};

    // Write to block 0
    result = iowriteat(blkio, 0, write_buf, 512);
    assert(result == 512);
    kprintf("\n✅ vioblk write to block 0\n");

    // Read back block 0
    result = ioreadat(blkio, 0, read_buf, 512);
    assert(result == 512);
    assert(strcmp(write_buf, read_buf) == 0);
    kprintf("\n✅ vioblk read matches write\n");

    ioclose(blkio);
    kprintf("\n=== 😈😈😈😈😈 vioblk test passed!!! 😈😈😈😈😈😈 ===\n");

    thread_exit();  // properly terminate the thread
}

void complex_test_thread(void) {
    struct io *blkio;
    int result = open_device("vioblk", 0, &blkio);
    assert(result == 0);
    kprintf("\n✅ vioblk device opened\n");

    unsigned long blksz;
    result = ioctl(blkio, GETBLKSZ, &blksz);
    assert(result == 0);
    kprintf("\n✅ Block size: %lu bytes\n", blksz);

    unsigned long long total_capacity;
    result = ioctl(blkio, GETEND, &total_capacity);
    assert(result == 0);
    kprintf("\n✅ Total capacity: %llu bytes\n", total_capacity);
    assert(total_capacity >= blksz * 3);  // ensure room for 3 blocks

    // Block 0 test
    char write_buf1[512], read_buf1[512];
    memset(write_buf1, 'A', sizeof(write_buf1));
    memset(read_buf1, 1, sizeof(read_buf1));

    result = iowriteat(blkio, 0, write_buf1, sizeof(write_buf1));
    assert(result == sizeof(write_buf1));
    kprintf("\n✅ vioblk write to block 0\n");

    result = ioreadat(blkio, 0, read_buf1, sizeof(read_buf1));
    assert(result == sizeof(read_buf1));
    assert(memcmp(write_buf1, read_buf1, sizeof(write_buf1)) == 0);
    kprintf("\n✅ vioblk read back from block 0 matches write\n");

    // Block 1 test
    char write_buf2[512], read_buf2[512];
    memset(write_buf2, 'B', sizeof(write_buf2));
    memset(read_buf2, 1, sizeof(read_buf2));

    result = iowriteat(blkio, blksz, write_buf2, sizeof(write_buf2));
    assert(result == sizeof(write_buf2));
    kprintf("\n✅ vioblk write to block 1\n");

    result = ioreadat(blkio, blksz, read_buf2, sizeof(read_buf2));
    assert(result == sizeof(read_buf2));
    dump_buffer("write_buf2", write_buf2, sizeof(write_buf2));
    dump_buffer("read_buf2", read_buf2, sizeof(read_buf2));
    assert(memcmp(write_buf2, read_buf2, sizeof(write_buf2)) == 0);
    kprintf("\n✅ vioblk read back from block 1 matches write\n");

    // Multi-block test (block 2 + 3)
    char multi_write[1024], multi_read[1024];
    for (int i = 0; i < sizeof(multi_write); i++)
        multi_write[i] = '0' + (i % 10);
    memset(multi_read, 0, sizeof(multi_read));

    result = iowriteat(blkio, 2 * blksz, multi_write, sizeof(multi_write));
    assert(result == sizeof(multi_write));
    kprintf("\n✅ vioblk multi-block write to block 2 & 3\n");

    result = ioreadat(blkio, 2 * blksz, multi_read, sizeof(multi_read));
    assert(result == sizeof(multi_read));
    assert(memcmp(multi_write, multi_read, sizeof(multi_write)) == 0);
    kprintf("\n✅ vioblk multi-block read matches write\n");

    ioclose(blkio);
    kprintf("\n=== 😈😈😈 vioblk complex test passed!!! 😈😈😈\n");

    thread_exit();
}

void vioblk_multi_rw_test(void) {
    struct io *blkio;
    int result = open_device("vioblk", 0, &blkio);
    assert(result == 0);
    kprintf("✅ Opened vioblk device\n");

    unsigned long blksz;
    result = ioctl(blkio, GETBLKSZ, &blksz);
    assert(result == 0);
    assert(blksz == 512);
    kprintf("✅ Block size = %lu\n", blksz);

    unsigned long long capacity;
    result = ioctl(blkio, GETEND, &capacity);
    assert(result == 0);
    assert(capacity >= blksz * 4); // test needs 4 blocks minimum
    kprintf("✅ Total capacity = %llu\n", capacity);

    // Buffers
    char wbufA[512], rbufA[512];
    char wbufB[512], rbufB[512];
    char wbufC[512], rbufC[512];

    memset(wbufA, 'A', 512);
    memset(wbufB, 'B', 512);
    memset(wbufC, 'C', 512);
    memset(rbufA, 0, 512);
    memset(rbufB, 0, 512);
    memset(rbufC, 0, 512);

    // Write to 3 blocks
    result = iowriteat(blkio, 0 * blksz, wbufA, blksz); assert(result == blksz);
    result = iowriteat(blkio, 1 * blksz, wbufB, blksz); assert(result == blksz);
    result = iowriteat(blkio, 2 * blksz, wbufC, blksz); assert(result == blksz);
    kprintf("✅ Multiple writes succeeded\n");

    // Read from same 3 blocks
    result = ioreadat(blkio, 0 * blksz, rbufA, blksz); assert(result == blksz);
    result = ioreadat(blkio, 1 * blksz, rbufB, blksz); assert(result == blksz);
    result = ioreadat(blkio, 2 * blksz, rbufC, blksz); assert(result == blksz);
    kprintf("✅ Multiple reads succeeded\n");

    // Check correctness
    assert(memcmp(wbufA, rbufA, blksz) == 0);
    assert(memcmp(wbufB, rbufB, blksz) == 0);
    assert(memcmp(wbufC, rbufC, blksz) == 0);
    kprintf("✅ Data read back matches written data on all blocks!\n");

    ioclose(blkio);
    kprintf("🎉 MULTI RW TEST PASSED!\n");

    thread_exit(); // end the test thread
}

void main(void) {
    // int pass = complex_test();
    // assert(pass);
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

    uart_attach((void*)UART0_MMIO_BASE, UART0_INTR_SRCNO);

    //  Spawn the complex test as a thread!
    thread_spawn("vioblk_test", vioblk_multi_rw_test);

    // Let the thread run
    // while (1) {
         thread_yield();
    // }
}
