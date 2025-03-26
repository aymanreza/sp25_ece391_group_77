// viorng.c - VirtIO rng device
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
#include "intr.h"
#include "console.h"
#include "thread.h"// add for cp3


// INTERNAL CONSTANT DEFINITIONS
//


#ifndef VIORNG_BUFSZ
#define VIORNG_BUFSZ 256
#endif


#ifndef VIORNG_NAME
#define VIORNG_NAME "rng"
#endif


#ifndef VIORNG_IRQ_PRIO
#define VIORNG_IRQ_PRIO 1
#endif


// INTERNAL TYPE DEFINITIONS
//


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


        // The first descriptor is a regular descriptor and is the one used in
        // the avail and used rings.


        struct virtq_desc desc[1];
         
    } vq;
    struct condition ready_not_empty;
//add this part here and check if this work


   
       


    // bufcnt is the number of bytes left in buffer. The usable bytes are
    // between buf+0 and buf+bufcnt. (We read from the end of the buffer.)


    unsigned int bufcnt;
    char buf[VIORNG_BUFSZ];
};


// INTERNAL FUNCTION DECLARATIONS
//


static int viorng_open(struct io ** ioptr, void * aux);
static void viorng_close(struct io * io);
static long viorng_read(struct io * io, void * buf, long bufsz);
static void viorng_isr(int irqno, void * aux);


// EXPORTED FUNCTION DEFINITIONS
//


// Attaches a VirtIO rng device. Declared and called directly from virtio.c.


// Inputs:  volatile struct virtio_mmio_regs * regs - this is the pointer for the MMIO register
//int irqno - The interrupt request number for the rng device
// Outputs: None
// Description/Side Effects: It initialize and attaches from the RNG device.
// It get the device operation and set up the queue and register of the device of the system
//The side effect the device regsiter allocate memory and enable interrupt.  
void viorng_attach(volatile struct virtio_mmio_regs * regs, int irqno)
{
    //           FIXME add additional declarations here if needed


    virtio_featset_t enabled_features, wanted_features, needed_features;
    int result;
   
    assert (regs->device_id == VIRTIO_ID_RNG);


    // Signal device that we found a driver
    regs->status |= VIRTIO_STAT_DRIVER;


    // fence o,io
    __sync_synchronize();


    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    result = virtio_negotiate_features(regs, enabled_features, wanted_features, needed_features);


    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }






    //FIXME Finish viorng initialization here!
    struct viorng_device *viorng = kmalloc(sizeof(struct viorng_device)); //allocate memory for the device structure
    static const struct iointf viorng_iointf =
    {
        .close = &viorng_close, // this will close to the rng device
        .read = &viorng_read // this will read random bytes from device
    };
    //this will stroe the register to the base address and number in the decvice  
    viorng->regs = regs;
    viorng->irqno = irqno;




    virtio_attach_virtq(regs, 0, 1, (uint64_t)&viorng->vq.desc,(uint64_t)&viorng->vq.used, (uint64_t)&viorng->vq.avail); ///this will attach the queue for the communication with the device
   
    viorng->vq.avail.ring[0] = 0; //this will intialize the ring buffer to 0
    ioinit0(&viorng->io, &viorng_iointf); //it initalize the interface to the define operation


    //this will set up the descriptor with the buffer size and address
    viorng->vq.desc->len =VIORNG_BUFSZ;
    viorng->vq.desc->addr =(uint64_t)viorng->buf;
    viorng->vq.desc->flags =VIRTQ_DESC_F_WRITE;
   
    // fence o,oi
    regs->status |= VIRTIO_STAT_DRIVER_OK;


    viorng->instno = register_device(VIORNG_NAME, viorng_open, viorng);  //this will get the regiter device in the system and will get the number
    // fence o,oi
    __sync_synchronize();
}
// Inputs:  struct io ** ioptr -  this will pointer to the io structure that will be assigned to the open viorng device
// void * aux - this will pointer to the viorng device that represnet the viorng
// Outputs: return 0 if it sucesses
// Description/Side Effects: It will intiailize the RNG device, reset the virtqueue, will know the device and will enable the interrupt.
//The side effect is that the devie regsiter and will enable the communication form the device.
int viorng_open(struct io ** ioptr, void * aux) {
    struct viorng_device *viorng = aux; //this will retrieve the device from the aux


    //this will reset the used and available index to ensure the correct states
    viorng->vq.used.idx=0;
    viorng->vq.avail.idx=0;


   
    condition_init(&viorng->ready_not_empty, "hello");
    viorng->regs->status =  viorng->regs->status | VIRTIO_STAT_ACKNOWLEDGE; //This will acknowledge the device and set to correct status
    *ioptr = ioaddref(&viorng->io); //this will provide the refrence to the I/O device
    virtio_enable_virtq(viorng->regs, 0); //this will be enable to communicate
    enable_intr_source(viorng->irqno, VIORNG_IRQ_PRIO, viorng_isr, viorng); //this will enable the interrupt for the device
    return 0;
}
// Inputs:  
// struct io *io - the pointer to the io with the viorng device is to be closed
// Outputs: none
// Description/Side Effects: It will close the rng device by resetting the virtqueue, clear the interupt,
//and disable the interrupts. The side effects is a device regsiter and will disable the interrupt.
void viorng_close(struct io * io)
{
    //           FIXME your code here
    struct viorng_device * const viorng = (void*)io - offsetof(struct viorng_device, io); // this will get the device structure from the io pointer
    assert(iorefcnt(io) == 0); // make sure that refdrence is zero before close the device
    virtio_reset_virtq(viorng->regs,0); //reset the queue to clear the states
    viorng->regs->interrupt_status = 0; //thsi will clear the status register to prevent interrupt
    disable_intr_source(viorng->irqno); // this will disable the interrupt for the rng device
}
// Inputs:  
// struct io *io - the pointer to the io with the viorng device is to be closed
//long bufsz -The maximum number of bytes read from the receive buffer
// Outputs: return i- the number of bytes sucessful read from the device
// Description/Side Effects: The read ramdom from the viorng rng device and stoere in the buffer.
//The function ensure  synchronization, wait for availability, and copies the bytes from the buffer tot he user buffer.
//The side effect is that the device register and synchronizes memory operations.
long viorng_read(struct io * io, void * buf, long bufsz) {
    //           FIXME your code here
    struct viorng_device * const viorng = (void*)io - offsetof(struct viorng_device, io); //  this will get the device structure from the io pointer
    char* buf_start = buf; //this is the buffer pointer to the chacter pointert for the byte wise operation
    viorng->vq.avail.idx++; //this will increment the aviable index
   
    __sync_synchronize(); //make sure that the scnyronized before contiuning
    virtio_notify_avail(viorng->regs, 0);  // this will notfiy the device that the new data us avaible for the queue
    int pie= disable_interrupts();
    while (viorng->vq.used.idx != viorng->vq.avail.idx)
    {
    condition_wait(&viorng->ready_not_empty);
    }
    restore_interrupts(pie);


   
    int i=0;
    while(i<VIORNG_BUFSZ) // check the bytes form the buffer into the user provided buffer
    {
     buf_start[i] = viorng->buf[i]; // copy the bytes
     i++;
     if(i == bufsz) //check if the buffer size is equal
     {
        break;
     }
    }
    return i; //return the number of bytes read
}
// Inputs:  
// int irqno - The interrupt request number for the rng device
// void * aux - this will pointer to the viorng device that represnet the viorng
// Outputs: None
// Description/Side Effects: This will handle the interrupt for the rng device, update the buffer count
//and will acknoledge the interrupt to prevent trigging. The side effect is the device register and update the buffer.
void viorng_isr(int irqno, void * aux) {
    //           FIXME your code here
    struct viorng_device *viorng = aux; //this will retrieve the device from the aux
    viorng->bufcnt = VIORNG_BUFSZ; // this will set the buffer count to indicate if it full
    viorng->regs->interrupt_ack = viorng->regs->interrupt_status; // this will anknowledge the interrupt to write the interrupt status
    condition_broadcast(&viorng->ready_not_empty);
    }
