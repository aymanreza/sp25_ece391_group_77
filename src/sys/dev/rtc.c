// rtc.c - Goldfish RTC driver
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "rtc.h"
#include "device.h"
#include "ioimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

#ifdef RTC_TRACE
#define TRACE
#endif

#ifdef RTC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "rtc.h"
#include "device.h"
#include "ioimpl.h"
#include "console.h"
#include "string.h"
#include "heap.h"

#include "error.h"

#include <stdint.h>

// INTERNAL TYPE DEFINITIONS
// 

struct rtc_regs {
    uint32_t low; //The lower 32 bits of the real time counter 
    uint32_t high; //The higer 32 bits of the real time counter 
};

struct rtc_device {
    // TODO
    volatile struct rtc_regs *regs; //It is a pointer to the RTC register 
    struct io io; //The I/O interface structure for this device 
    int instno; //The number of device of the registration 
};

// INTERNAL FUNCTION DEFINITIONS
//

static int rtc_open(struct io ** ioptr, void * aux); 
static void rtc_close(struct io * io);  
static int rtc_cntl(struct io * io, int cmd, void * arg); 
static long rtc_read(struct io * io, void * buf, long bufsz);
static uint64_t read_real_time(struct rtc_regs * regs);

// EXPORTED FUNCTION DEFINITIONS
// 

// Inputs: 
//void *mmio_base- The base memory is mapped to the I/O adress of the RTC device 
// Outputs: None
// Description/Side Effects: This function sets up the device by allocating memory, initializing the I/O interface,
//and registering it to enable usage through the provided interface.
//The side effect isthat it allocate memory for the rtc device using kcalloc().
void rtc_attach(void * mmio_base) {
    // TODO
    static const struct iointf intf = {
        .close = rtc_close, //The close function to the interface 
        .read  = rtc_read, //The read function to the interface 
        .cntl  = rtc_cntl //The control function to the interface
    };
    struct rtc_device *rtc;
    rtc = kcalloc(1, sizeof(struct rtc_device)); // It allocate the memory for RTC device struture
    rtc->regs = mmio_base; //The RTC register pointer with the base address 
    ioinit0(&rtc->io, &intf); // The I/O structure with the interface 
    rtc->instno = register_device("rtc", rtc_open, rtc); // The register of the device with the system 
    
}
// uint32_t add(uint32_t a, uint32_t b)
// Inputs:
//struct io **ioptr - this will pointer to the io structure that will be assigned to the open RTC device
//void *aux -This will pointer to the RTC device that represnet the UART
// Outputs: this will return 0 when there is sucessses
// Description/Side Effects: This function opens the device by creating an rtc_device from the provided data,
// setting the I/O pointer to the device, and returning success.
// The side effect is that it point to the RTC decvice to the I/O structure 
// and it increment the refrence count to the RTC


int rtc_open(struct io ** ioptr, void * aux) {
    // TODO
    struct rtc_device *rtc = aux; //the rtc_device is create to the aux 
    *ioptr = ioaddref(&rtc->io); //It set IO pojtnter the refrence form the device
    return 0; //return the sucesses 
}

// Inputs: 
// struct io *io -The pointer to the io structure with RTC device 
// Outputs: None 
// Description/Side Effects: This function closes the device by retrieving it from the I/O pointer and ensuring the reference count is zero.
// It then frees the allocated memory for the device.
// The side effect is call if the refrence remain which end the execution. 

void rtc_close(struct io * io) {
    // TODO
    struct rtc_device *rtc = (void *)io - offsetof(struct rtc_device, io); // get the rtc_device from the io pointer
    assert(iorefcnt(io) == 0); //make sure that he refrence is zero before doing the freeing memory 
    kfree(rtc); ///this will free the meory from the device 
}
// Inputs:
// struct io *io -The pointer to the io structure with RTC device 
// int cmd - The will identify specify for the operation 
//void *arg - These argument are for the command tht are uused in the implementation
// Outputs: return 8 to incidate the block size of the byte. It return `-ENOTSUP` if the command is not support.
// Description: This function handles the control commands of the device by checking if the command is to get the block size, returning 8 bytes.
// If the command is not for the block size, it returns an operation error for any other command.
// Side Effects: NOne
 
int rtc_cntl(struct io * io, int cmd, void * arg) {
    // TODO
    if (cmd == IOCTL_GETBLKSZ) { ///check if command get the block size 
        return 8; //return the byte size 8 because of the blcok size 
    }
    return -ENOTSUP; //this will end if there is other command 
}
// Inputs: 
//  struct io *io - The pointer to the io structure with RTC device 
//  void *buf - The pointer to the buffer to the real time value that would be stored.
//long bufsz - This is the size of the buffer 
// Outputs: return the success which the number of byte read. It also return the bufz to zero
// and return `-EINVAL` if it smaller of the 64 bit  
// Description/ide Effects: This function reads the real-time value from the device by checking if the buffer size is valid.
// It also reads the current time from the device registers, copies the value to the provided buffer,
// and returns the number of bytes read.
// The side effect is that it read the real time clock value and copy the time value into the buffer.

long rtc_read(struct io * io, void * buf, long bufsz) {
    // TODO
    struct rtc_device *rtc = (void *)io - offsetof(struct rtc_device, io);
    
    if (bufsz == 0) { //check if the buffer is zero
        return 0; // return 0 if it true 
    }
    
    if (bufsz < sizeof(uint64_t)) { //check if the  buffer sze is saller than 64 bit 
        return -EINVAL; // return that it invlaid 
    }

    uint64_t temp; 
    temp = read_real_time((struct rtc_regs *)rtc->regs); //this will read the real time value from the regsiter 
    memcpy(buf, &temp, sizeof(uint64_t)); //this will copy value to the buffer 

    return sizeof(uint64_t); // this will retunr the number of byte read 
}
// Inputs:
//   struct rtc_regs *regs - This will point to the RTC register
// Outputs: this will return 64-bit real-time counter value
// Description/Side Effects: This function reads the real-time counter from the register by retrieving the lower and higher values.
// Then it combines the high and low values to return the complete count.
// The side effect is thta it read the memory mapped to the RTC

uint64_t read_real_time(struct rtc_regs * regs) {
    // TODO
    uint32_t lo, high;
    lo = regs->low; //The lower 32 bits of the real time counter 
    high = regs->high; //The higher 32 bits of the real time counter 
    
    return ((uint64_t)high << 32) | lo; // This will combine of the high and low into a 64 bit 
}
