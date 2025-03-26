
// uart.c - NS8250-compatible uart port
// 
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//


#ifdef UART_TRACE
#define TRACE
#endif


#ifdef UART_DEBUG
#define DEBUG
#endif


#include "conf.h"
#include "assert.h"
#include "uart.h"
#include "device.h"
#include "intr.h"
#include "heap.h"


#include "ioimpl.h"
#include "console.h"


#include "error.h"


#include <stdint.h>
#include "thread.h" // add for cp3


// COMPILE-TIME CONSTANT DEFINITIONS
//


#ifndef UART_RBUFSZ
#define UART_RBUFSZ 64
#endif


#ifndef UART_INTR_PRIO
#define UART_INTR_PRIO 1
#endif


#ifndef UART_NAME
#define UART_NAME "uart"
#endif


// INTERNAL TYPE DEFINITIONS
// 


struct uart_regs {
    union {
        char rbr; // DLAB=0 read
        char thr; // DLAB=0 write
        uint8_t dll; // DLAB=1
    };
   
    union {
        uint8_t ier; // DLAB=0
        uint8_t dlm; // DLAB=1
    };
   
    union {
        uint8_t iir; // read
        uint8_t fcr; // write
    };


    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
};


#define LCR_DLAB (1 << 7)
#define LSR_OE (1 << 1)
#define LSR_DR (1 << 0)
#define LSR_THRE (1 << 5)
#define IER_DRIE (1 << 0)
#define IER_THREIE (1 << 1)


struct ringbuf {
    unsigned int hpos; // head of queue (from where elements are removed)
    unsigned int tpos; // tail of queue (where elements are inserted)
    char data[UART_RBUFSZ];
};


struct uart_device {
    volatile struct uart_regs * regs;
    int irqno;
    int instno;


    struct io io;


    unsigned long rxovrcnt; // number of times OE was set


    struct ringbuf rxbuf;
    struct ringbuf txbuf;
    struct condition rxbuf_not_empty;
    struct condition rxtuf_not_empty;


};


// INTERNAL FUNCTION DEFINITIONS
//


static int uart_open(struct io ** ioptr, void * aux);
static void uart_close(struct io * io);
static long uart_read(struct io * io, void * buf, long bufsz);
static long uart_write(struct io * io, const void * buf, long len);


static void uart_isr(int srcno, void * driver_private);


static void rbuf_init(struct ringbuf * rbuf);
static int rbuf_empty(const struct ringbuf * rbuf);
static int rbuf_full(const struct ringbuf * rbuf);
static void rbuf_putc(struct ringbuf * rbuf, char c);
static char rbuf_getc(struct ringbuf * rbuf);


// EXPORTED FUNCTION DEFINITIONS
// 


void uart_attach(void * mmio_base, int irqno) {
    static const struct iointf uart_iointf = {
        .close = &uart_close,
        .read = &uart_read,
        .write = &uart_write
    };


    struct uart_device * uart;


    uart = kcalloc(1, sizeof(struct uart_device));


    uart->regs = mmio_base;
    uart->irqno = irqno;


    ioinit0(&uart->io, &uart_iointf);


    // Check if we're trying to attach UART0, which is used for the console. It
    // had already been initialized and should not be accessed as a normal
    // device.


    if (mmio_base != (void*)UART0_MMIO_BASE) {


        uart->regs->ier = 0;
        uart->regs->lcr = LCR_DLAB;
        // fence o,o ?
        uart->regs->dll = 0x01;
        uart->regs->dlm = 0x00;
        // fence o,o ?
        uart->regs->lcr = 0; // DLAB=0


        uart->instno = register_device(UART_NAME, uart_open, uart);


    } else
        uart->instno = register_device(UART_NAME, NULL, NULL);
}




// Inputs:
// struct io **ioptr - this will pointer to the io structure that will be assigned to the open Uart device
// void *aux - This will pointer to the Uart devie that represnet the UART
// Outputs: this will return the 0 when it succesullly or if the urat device is already in use
// Description/Side Effects: This function initializes and opens the UART device by resetting the receive and transmit buffers, flushing data from the hardware buffer,
// enabling the data-ready interrupt, registering the UART interrupt service routine with a specified priority,
// incrementing the reference count to indicate that the UART is open, and setting the provided I/O pointer before returning success.
// The side effect will be able to enable recevice interrupt for the Uart.


int uart_open(struct io ** ioptr, void * aux) {
    struct uart_device * const uart = aux;


    trace("%s()", __func__);


    if (iorefcnt(&uart->io) != 0)
        return -EBUSY;
   
    // Reset receive and transmit buffers
   
    rbuf_init(&uart->rxbuf);
    rbuf_init(&uart->txbuf);
    condition_init(&uart->rxbuf_not_empty, "rxbuf");
    condition_init(&uart->rxtuf_not_empty, "txbuf");




    // Read receive buffer register to flush any stale data in hardware buffer


    uart->regs->rbr; // forces a read because uart->regs is volatile


    // FIXME your code goes here
    uart->regs->ier = IER_DRIE; // notify when the data is ready to be read
    enable_intr_source(uart->irqno, UART_INTR_PRIO, uart_isr, uart); //the register of the UART interrupt routine with a given priority
    ioaddref(&uart->io);  //Imcrement the reference count to know that the UART is open  
    *ioptr = ioaddref(&uart->io); //set the provided IO pointer to the UART with incremented  
    return 0; //return to indiciate that the UART have been sucessful
}
// Inputs:
// struct io *io - the pointer to the io with the Uart device is to be closed
// Outputs: none
// Description/Side Effects: This function closes the UART device by disabling the data-ready and transmit interrupts,disabling the UART interrupt source,
//and resetting the receive and transmit buffers to their default states if there are no active references remaining.
//The side effect will be able to disable interrupt for the Uart.


void uart_close(struct io * io) {
    struct uart_device * const uart =
        (void*)io - offsetof(struct uart_device, io);


    trace("%s()", __func__);
    assert (iorefcnt(io) == 0);
   


    // FIXME your code goes here
    uart->regs->ier = uart->regs->ier & ~(IER_DRIE | IER_THREIE); // disable the data ready and transmit interrupts
    disable_intr_source(uart->irqno); //disable the UART interrupt source
    if (iorefcnt(io) == 0) { //check if there any active reference remains
        rbuf_init(&uart->rxbuf); //resets the receive buffer to defualt
        rbuf_init(&uart->txbuf); //resets the  transmit buffer to defualt
}
}


// Inputs:
// struct io *io - The pointer to the io structure with urat device
// void *buf - it will pointer to the buffer where it will store the data
//long bufsz -The maximum number of bytes read from the receive buffer
// Outputs: The counter would be the number of bytes sucessfully read from the Uart buffer
// Description/Side Effects: This function reads data from the UART device by retrieving characters from the receive buffer.
// The data is read until the requested number of bytes is reached or the buffer is empty,
// re enabling the data ready interrupt afterward, and returning the number of bytes successfully read.
// The side effect is that it will read the data from the Uart buffer and re enable the data interrupt.


long uart_read(struct io * io, void * buf, long bufsz)
{
    // FIXME your code goes here
    struct uart_device * const uart = (void*)io - offsetof(struct uart_device, io); // get the uart_device from the io pointer
    if (bufsz <= 0) //check if it invalid
    {
        return 0;  //return 0 if it invalid
    }
   
    // while (rbuf_empty(&uart->rxbuf)) //check if the recevice buffer have data
    // {
    //     condition_wait(&uart->rxbuf_not_empty);
    // }
    int pie= disable_interrupts();
    while (rbuf_empty(&uart->rxbuf))
    {
    condition_wait(&uart->rxbuf_not_empty);
    }
    restore_interrupts(pie);


    char *chacter_buf = buf; // make a pointer to the chacter_buf
    long counter = 0; //counter for bytes read
    while (counter < bufsz && !rbuf_empty(&uart->rxbuf)) //check if the recevice buffer have data
    {
        chacter_buf[counter++] = rbuf_getc(&uart->rxbuf);//the will get from recivce buffer
    }
    uart->regs->ier = uart->regs->ier | IER_DRIE; //this will reenable the data ready interrupt  
    return counter;  //the number of byte that have sucessfully read
}
// Inputs:
//struct io *io -The pointer to the io structure with urat device
// const void *buf - The pointer to the buffer will have the data to be trnsmitted.
//long len - The number of byes to the write to the Uart buffer.
// Outputs: The number of bytes which are successfuly written from the uart buffer
// Description/Side Effects: This function writes data to the UART device by placing characters from the provided buffer into the transmit buffer until the specified length
// is reached or the buffer is full, enabling the transmit register empty interrupt, and returning the number of bytes successfully written.
// The side effect is the write to the Uart trsamit buffer and enable the data interrupt.


long uart_write(struct io * io, const void * buf, long len)
{
    // FIXME your code goes here
    struct uart_device * const uart = (void*)io - offsetof(struct uart_device, io); // get the uart_device from the io pointer
    if (len <= 0) //check if it invalid
    {
        return 0;  //return 0 if it invalid
    }
    const char *chacter_buf = buf; // make a array to the chacter_buf
    long counter = 0;  //counter for byte written
    // while (rbuf_full(&uart->txbuf)) //check if the trasmit buffer is full
    // {
    //     condition_wait(&uart->rxbuf_not_empty);
    // }
    int pie= disable_interrupts();
    while (rbuf_full(&uart->txbuf))
    {
    condition_wait(&uart->rxtuf_not_empty);
    }
    restore_interrupts(pie);
    while (counter < len && !rbuf_full(&uart->txbuf)) // this will write data when the buffer have space
    {
    rbuf_putc(&uart->txbuf, chacter_buf[counter++]); //the data will go in the tramsit buffer
    uart->regs->ier = uart->regs->ier | IER_THREIE; //this will enable the trasmit resiger empty for interrupt


    }


    return counter; //the number of byte that have sucessfully written
}
// Inputs:
// int srcno -This is the interrupt source number
// void *aux -This will pointer to the Uart devie that represnet the UART
// Outputs: None
// Description/Side Effects: This function handles UART interrupts by checking the interrupt type, reading and storing data into the receive buffer
// if the data ready interrupt is triggered, or transmitting data from the transmit buffer.
// The transmit buffer empty interrupt is triggered, and disabling the transmit interrupt if the buffer is empty.
// The side effect is that read the incoming data and store it in the rxbf and it send the data from txbuf to the Uart.
//It also disable the transmit of the interrupt when the buffer is empty


void uart_isr(int srcno, void * aux)
{
    struct uart_device * const uart = aux; // get the aux to uart_device for interrupt
    uint8_t inter_id = uart->regs->iir;  // this will read the interruppt register
    const uint8_t data_read_inter = 0x4;
    const uint8_t TX_buf_empty_inter = 0x2;
    if ((inter_id & data_read_inter) == data_read_inter) // check if the data read interrupt
    {
        while ((uart->regs->lsr & LSR_DR) && !rbuf_full(&uart->rxbuf)) //this will read the data whern the buffer is not full
        {
            rbuf_putc(&uart->rxbuf, uart->regs->rbr); //this will stores the recevice days in buffer
            condition_broadcast(&uart->rxbuf_not_empty);
        }
        if(rbuf_full(&uart->rxbuf))
        {
            uart->regs->ier = uart->regs->ier & ~IER_DRIE;
        }  
    }
    if ((inter_id & TX_buf_empty_inter) == TX_buf_empty_inter) // check if the transmit buffer empty interrupt
    {  
        while ((uart->regs->lsr & LSR_THRE) && !rbuf_empty(&uart->txbuf)) //this will transmit the data whern the buffer is not full
        {
            uart->regs->thr = rbuf_getc(&uart->txbuf); // this will send the data to hardware
            condition_broadcast(&uart->rxtuf_not_empty);
    }
        }
        if (rbuf_empty(&uart->txbuf)) // check if the buffer is empty after the trasmission
        {
            uart->regs->ier = uart->regs->ier & ~IER_THREIE; //this will disalable Transmit interrupt to prevent it been called again
        }
       
}




void rbuf_init(struct ringbuf * rbuf) {
    rbuf->hpos = 0;
    rbuf->tpos = 0;
}


int rbuf_empty(const struct ringbuf * rbuf) {
    return (rbuf->hpos == rbuf->tpos);
}


int rbuf_full(const struct ringbuf * rbuf) {
    return ((uint16_t)(rbuf->tpos - rbuf->hpos) == UART_RBUFSZ);
}


void rbuf_putc(struct ringbuf * rbuf, char c) {
    uint_fast16_t tpos;


    tpos = rbuf->tpos;
    rbuf->data[tpos % UART_RBUFSZ] = c;
    asm volatile ("" ::: "memory");
    rbuf->tpos = tpos + 1;
}


char rbuf_getc(struct ringbuf * rbuf) {
    uint_fast16_t hpos;
    char c;


    hpos = rbuf->hpos;
    c = rbuf->data[hpos % UART_RBUFSZ];
    asm volatile ("" ::: "memory");
    rbuf->hpos = hpos + 1;
    return c;
}




// The functions below provide polled uart input and output for the console.


#define UART0 (*(volatile struct uart_regs*)UART0_MMIO_BASE)


void console_device_init(void) {
    UART0.ier = 0x00;


    // Configure UART0. We set the baud rate divisor to 1, the lowest value,
    // for the fastest baud rate. In a physical system, the actual baud rate
    // depends on the attached oscillator frequency. In a virtualized system,
    // it doesn't matter.
   
    UART0.lcr = LCR_DLAB;
    UART0.dll = 0x01;
    UART0.dlm = 0x00;


    // The com0_putc and com0_getc functions assume DLAB=0.


    UART0.lcr = 0;
}


void console_device_putc(char c) {
    // Spin until THR is empty
    while (!(UART0.lsr & LSR_THRE))
        continue;


    UART0.thr = c;
}


char console_device_getc(void) {
    // Spin until RBR contains a byte
    while (!(UART0.lsr & LSR_DR))
        continue;
   
    return UART0.rbr;
}
