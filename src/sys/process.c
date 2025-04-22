// process.c - user process
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//



#ifdef PROCESS_TRACE
#define TRACE
#endif

#ifdef PROCESS_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "process.h"
#include "elf.h"
#include "fs.h"
#include "io.h"
#include "string.h"
#include "thread.h"
#include "riscv.h"
#include "trap.h"
#include "memory.h"
#include "heap.h"
#include "error.h"

// COMPILE-TIME PARAMETERS
//


#ifndef NPROC
#define NPROC 16
#endif


// INTERNAL FUNCTION DECLARATIONS
//


static int build_stack(void * stack, int argc, char ** argv);


// static void fork_func(struct condition * forked, struct trap_frame * tfr);

// INTERNAL GLOBAL VARIABLES
//


static struct process main_proc;


static struct process * proctab[NPROC] = {
    &main_proc
};

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

// helper functions
struct io * process_get_io(int fd){
    // checking the bounds of IO array
    if(fd < 0 || fd >= PROCESS_IOMAX){
        kprintf("FILE DESCRIPTOR OUT OF BOUNDS");
        return NULL;
    }

    struct process * cur_process = current_process(); // getting the currently running process
    struct io * process_io = cur_process->iotab[fd]; // recovering io
    return process_io; // returning io
}

void procmgr_init(void) {
    assert (memory_initialized && heap_initialized);
    assert (!procmgr_initialized);

    main_proc.idx = 0;
    main_proc.tid = running_thread();
    main_proc.mtag = active_mspace();
    thread_set_process(main_proc.tid, &main_proc);
    procmgr_initialized = 1;
}

int process_exec(struct io * exeio, int argc, char ** argv) {
    assert(exeio != NULL); // validating arugments

    // Get current process
    struct process *proc = current_process();
    assert(proc != NULL);
    
    // Reset memory space (clear old mappings and free physical pages)
    reset_active_mspace();
    // kprintf("Successfully Reset Memory Space...\n");

    // Load ELF executable (returns entry point or 0 on failure)
    void (*entry)(void);
    int ret = elf_load(exeio, &entry);
    if (ret < 0 || entry == NULL) {
        kprintf("ELF LOAD FAILED\n");
        thread_exit();
    }
    // kprintf("Successfully Loaded ELF...\n");

    // Allocate and build user stack
    void *stack = alloc_phys_page();
    if (stack == NULL) {
        kprintf("FAILED TO ALLOCATE STACK\n");
        thread_exit();
    }
    // kprintf("Successfully Allocated User Stack...\n");

    map_page(UMEM_END_VMA - PAGE_SIZE, stack, PTE_R | PTE_W | PTE_U);
    int stksz = build_stack(stack, argc, argv);
    if (stksz < 0) {
        kprintf("FAILED TO BUILD USER STACK\n");
        thread_exit();
    }
    // kprintf("Successfully Built User Stack...\n");

    // Set up trap frame
    struct trap_frame tf = {0};
    tf.sp = (void *)(UMEM_END_VMA - stksz); // user stack top
    kprintf("Stack size = %d\n", stksz);
    kprintf("Expected tf.sp = 0x%x\n", (uintptr_t)(UMEM_END_VMA - stksz));
    tf.ra = entry; // jump to ELF entry point
    tf.sepc = entry;  // program counter

    // Set argument registers
    tf.a0 = argc;
    tf.a1 = (long)(UMEM_END_VMA - PAGE_SIZE + ((char *)stack + PAGE_SIZE - stksz) - (char *)stack);
    // kprintf("Successfully Built Trapframe...\n");
    kprintf("JUMPING TO USER ENTRY = %p\n", entry);
    // Jump to user mode
    trap_frame_jump(&tf, get_scratch());

    // Should never return
    return -1;
}

int process_fork(const struct trap_frame * tfr) {
    return 0; 
}

void process_exit(void) {
    struct process *proc = current_process(); // getting current process
    if (proc == NULL) //if there is not process, exit thread
        thread_exit();

    // Panic if main thread exits
    if (running_thread() == 0) {
        panic("Main process exited");
    }

    // close all open I/O
    for (int i = 0; i < PROCESS_IOMAX; i++) {
        if (proc->iotab[i] != NULL) { //if table spot is used
            ioclose(proc->iotab[i]);   // safe to call even if already closed
            proc->iotab[i] = NULL;  //clear it
        }
    }

    // discard the memory space
    // discard_active_mspace();

    // remove from proctab
    if (proc->idx >= 0 && proc->idx < NPROC)
        proctab[proc->idx] = NULL;

    // free process if not static main_proc
    if (proc != &main_proc)
        kfree(proc);

    // exit thread
    thread_exit();
}

// INTERNAL FUNCTION DEFINITIONS
//

int build_stack(void * stack, int argc, char ** argv) {
    size_t stksz, argsz;
    uintptr_t * newargv;
    char * p;
    int i;

    // We need to be able to fit argv[] on the initial stack page, so _argc_
    // cannot be too large. Note that argv[] contains argc+1 elements (last one
    // is a NULL pointer).

    if (PAGE_SIZE / sizeof(char*) - 1 < argc)
        return -ENOMEM;
    
    stksz = (argc+1) * sizeof(char*);

    // Add the sizes of the null-terminated strings that argv[] points to.

    for (i = 0; i < argc; i++) {
        argsz = strlen(argv[i])+1;
        if (PAGE_SIZE - stksz < argsz)
            return -ENOMEM;
        stksz += argsz;
    }

    // Round up stksz to a multiple of 16 (RISC-V ABI requirement).

    stksz = ROUND_UP(stksz, 16);
    assert (stksz <= PAGE_SIZE);

    // Set _newargv_ to point to the location of the argument vector on the new
    // stack and set _p_ to point to the stack space after it to which we will
    // copy the strings. Note that the string pointers we write to the new
    // argument vector must point to where the user process will see the stack.
    // The user stack will be at the highest page in user memory, the address of
    // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
    // stack is given by `p - newargv'.

    newargv = stack + PAGE_SIZE - stksz;
    p = (char*)(newargv+argc+1);

    for (i = 0; i < argc; i++) {
        newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void*)p - (void*)stack);
        argsz = strlen(argv[i])+1;
        memcpy(p, argv[i], argsz);
        p += argsz;
    }

    newargv[argc] = 0;
    return stksz;
}

// void fork_func(struct condition * done, struct trap_frame * tfr) {
//     // switch to child’s memory space
//     switch_mspace(current_process()->mtag);

//     // Notify parent it can free trap frame
//     condition_broadcast(done);

//     // enter U-mode
//     trap_frame_jump(tfr, TP->stack_anchor - sizeof(struct trap_frame));
//     return;
// }