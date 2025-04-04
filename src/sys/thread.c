// thread.c - Threads
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//


#ifdef THREAD_TRACE
#define TRACE
#endif


#ifdef THREAD_DEBUG
#define DEBUG
#endif


#include "thread.h"


#include <stddef.h>
#include <stdint.h>


#include "assert.h"
#include "heap.h"
#include "string.h"
#include "riscv.h"
#include "intr.h"
#include "memory.h"
#include "error.h"


#include <stdarg.h>


// COMPILE-TIME PARAMETERS
//


// NTHR is the maximum number of threads


#ifndef NTHR
#define NTHR 16
#endif


#ifndef STACK_SIZE
#define STACK_SIZE 4000
#endif


// EXPORTED GLOBAL VARIABLES
//


char thrmgr_initialized = 0;


// INTERNAL TYPE DEFINITIONS
//


enum thread_state {
    THREAD_UNINITIALIZED = 0,
    THREAD_WAITING,
    THREAD_SELF,
    THREAD_READY,
    THREAD_EXITED
};


struct thread_context {
    uint64_t s[12];
    void * ra;
    void * sp;
};


struct thread_stack_anchor {
    struct thread * ktp;
    void * kgp;
};


struct thread {
    struct thread_context ctx;  // must be first member (thrasm.s)
    int id; // index into thrtab[]
    enum thread_state state;
    const char * name;
    struct thread_stack_anchor * stack_anchor;
    void * stack_lowest;
    struct thread * parent;
    struct thread * list_next;
    struct condition * wait_cond;
    struct condition child_exit;
    struct lock * lock_list; // added linked list of locks currently held by this thread
};


// INTERNAL MACRO DEFINITIONS
// 


// Pointer to running thread, which is kept in the tp (x4) register.


#define TP ((struct thread*)__builtin_thread_pointer())


// Macro for changing thread state. If compiled for debugging (DEBUG is
// defined), prints function that changed thread state.


#define set_thread_state(t,s) do { \
    debug("Thread <%s:%d> state changed from %s to %s by <%s:%d> in %s", \
        (t)->name, (t)->id, \
        thread_state_name((t)->state), \
        thread_state_name(s), \
        TP->name, TP->id, \
        __func__); \
    (t)->state = (s); \
} while (0)


// INTERNAL FUNCTION DECLARATIONS
//


// Initializes the main and idle threads. called from threads_init().


static void init_main_thread(void);
static void init_idle_thread(void);


// Sets the RISC-V thread pointer to point to a thread.


static void set_running_thread(struct thread * thr);


// Returns a string representing the state name. Used by debug and trace
// statements, so marked unused to avoid compiler warnings.


static const char * thread_state_name(enum thread_state state)
    __attribute__ ((unused));


// void thread_reclaim(int tid)
//
// Reclaims a thread's slot in thrtab and makes its parent the parent of its
// children. Frees the struct thread of the thread.


static void thread_reclaim(int tid);


// struct thread * create_thread(const char * name)
//
// Creates and initializes a new thread structure. The new thread is not added
// to any list and does not have a valid context (_thread_switch cannot be
// called to switch to the new thread).


static struct thread * create_thread(const char * name);


// void running_thread_suspend(void)
// Suspends the currently running thread and resumes the next thread on the
// ready-to-run list using _thread_swtch (in threasm.s). Must be called with
// interrupts enabled. Returns when the current thread is next scheduled for
// execution. If the current thread is TP, it is marked READY and placed
// on the ready-to-run list. Note that running_thread_suspend will only return if the
// current thread becomes READY.


static void running_thread_suspend(void);


// The following functions manipulate a thread list (struct thread_list). Note
// that threads form a linked list via the list_next member of each thread
// structure. Thread lists are used for the ready-to-run list (ready_list) and
// for the list of waiting threads of each condition variable. These functions
// are not interrupt-safe! The caller must disable interrupts before calling any
// thread list function that may modify a list that is used in an ISR.


static void tlclear(struct thread_list * list);
static int tlempty(const struct thread_list * list);
static void tlinsert(struct thread_list * list, struct thread * thr);
static struct thread * tlremove(struct thread_list * list);
// static void tlappend(struct thread_list * l0, struct thread_list * l1);


static void idle_thread_func(void);


// IMPORTED FUNCTION DECLARATIONS
// defined in thrasm.s
//


extern struct thread * _thread_swtch(struct thread * thr);


extern void _thread_startup(void);


// INTERNAL GLOBAL VARIABLES
//


#define MAIN_TID 0
#define IDLE_TID (NTHR-1)


static struct thread main_thread;
static struct thread idle_thread;


extern char _main_stack_lowest[]; // from start.s
extern char _main_stack_anchor[]; // from start.s


static struct thread main_thread = {
    .id = MAIN_TID,
    .name = "main",
    .state = THREAD_SELF,
    .stack_anchor = (void*)_main_stack_anchor,
    .stack_lowest = _main_stack_lowest,
    .child_exit.name = "main.child_exit"
};


extern char _idle_stack_lowest[]; // from thrasm.s
extern char _idle_stack_anchor[]; // from thrasm.s
// Inputs: None
// Outputs: None
// Description/Side Effects: It will initialize the ideal thread, which runs when there no other threads available.
//The idle thread execute and exited when it neeed. The side effect were the set up for the idle thread context
// which include the stack pointer and executions.
static struct thread idle_thread = {
    .id = IDLE_TID,
    .name = "idle",
    .state = THREAD_READY,
    .parent = &main_thread,
    .stack_anchor = (void*)_idle_stack_anchor,
    .stack_lowest = _idle_stack_lowest,
    .ctx.sp = _idle_stack_anchor,
    .ctx.ra = &_thread_startup,
    // FIXME your code goes here
    .ctx.s[8]= (uint64_t)&thread_exit, // this will set the argument to the thread_starup
    .ctx.s[9] = (uint64_t)&idle_thread_func // will used to functon execute
   
};


static struct thread * thrtab[NTHR] = {
    [MAIN_TID] = &main_thread,
    [IDLE_TID] = &idle_thread
};


static struct thread_list ready_list = {
    .head = &idle_thread,
    .tail = &idle_thread
};


// EXPORTED FUNCTION DEFINITIONS
//


int running_thread(void) {
    return TP->id;
}


void thrmgr_init(void) {
    trace("%s()", __func__);
    init_main_thread();
    init_idle_thread();
    set_running_thread(&main_thread);
    thrmgr_initialized = 1;
}
// Inputs: name-  the name of the name thread.
//entry-function pointer for the thread entry function
// ...- argument to pass to the new threads
// Outputs: None return the thread id of the created thread or -EMTHR  if it fails  
// Description/Side Effects: create a new thread, initalize it context, and to the ready lists.
//The new thread will start excusion with the entry function.
//The side effect is the new thread, red list, and set up the tread's stack and regsiters.


int thread_spawn (
    const char * name,
    void (*entry)(void),
    ...)
{
    struct thread * child;
    va_list ap;
    int pie;
    int i;


    child = create_thread(name);


    if (child == NULL)
        return -EMTHR;


    set_thread_state(child, THREAD_READY);


    pie = disable_interrupts();
    tlinsert(&ready_list, child);
    restore_interrupts(pie);


    // FIXME your code goes here
    // filling in entry function arguments is given below, the rest is up to you


    child->ctx.ra = &_thread_startup; //this will set up the return adresss to the startup function which is properly initalize in the thread
    child->ctx.sp = child->stack_anchor;  // this will get the stack pointer the 16 bytes boundry to the correct stack algiment
    child->ctx.s[8] = (uint64_t)&thread_exit; // thid will set the thread to the exit handle
    child->ctx.s[9] = (uint64_t)entry;  // this will store the fuction pointeer in the regsiter
    va_start(ap, entry);
    for (i = 0; i < 8; i++)
        child->ctx.s[i] = va_arg(ap, uint64_t);
    va_end(ap);
   
    return child->id;
}
// Inputs: none
// Outputs: none
// Description/Side Effects: It will end the current and main tread exist, it will halt. The function interates through all locks in the lock_list and releases them. 
// It set the tread state to the exited, notfiy the parent, and suspend to switch to another thread. The side effect is thread states, signal for the parent thread and suspend execution
void thread_exit(void) {
    // FIXME your code goes here
   if(TP->id == MAIN_TID) // check if the main attmpet exit which will end the program
   {
    halt_success(); // this will not continue if the main thread exist
   }

   // this is the head of the lock_list
   struct lock *lock = TP->lock_list;

   // move through all elements of the lock_list and call lock_release on all of the lock in it
    while (lock != NULL) {
        struct lock *next = lock->next;
        lock_release(lock);
        lock = next;
    }

   set_thread_state(TP, THREAD_EXITED); // this will check if the current thread exist
    condition_broadcast(&TP->parent->child_exit); //notify the parent thread if the thread exist
    running_thread_suspend(); // this will suspend the execution and change to the avaibale thread
    halt_failure(); // check if the control reach here, which mean that thread system have failed
}


void thread_yield(void) {
    trace("%s() in <%s:%d>", __func__, TP->name, TP->id);
    running_thread_suspend();
}
// Inputs: tid - it is the child thread to wait for any child
// Outputs: uthsi will return the thread ID of the exited chld, -EINVAL if no child exists, or invalid is given.
// Description/Side Effects: It will wait form the cild thread to exit and then reclaim it resoruces.
//If tid is zero, it will wait for any child thread to exit. Otherwise, it will wait for the specific child.
//The side effect is calling the tread until the child thread exits.
//  None
int thread_join(int tid) {
    // FIXME your code goes here
    struct thread *child = NULL;
    int pie; // this iwll stroe the interupt states
    pie = disable_interrupts(); // will make sure the remain consistent
    if(tid==0)
    {
        int has_children = 0;
       
            for (int i = 0; i < NTHR; i++) // check through the thread to find any child of the current thread
            {
                if (thrtab[i]->parent == TP)
                {
                    has_children = 1;
                    break;
                   
                }
            }
            //check if the have no childern which give out an error
            if (has_children == 0)
             {
                return -EINVAL;
            }
           
            condition_wait(&TP->child_exit); // thsi wlll wait on the child thread to exit


            //this will find the first exited child and will recalim the resources
            for (int i = 0; i < NTHR; i++) {
                if (thrtab[i] && thrtab[i]->parent == TP && thrtab[i]->state == THREAD_EXITED) {
                    thread_reclaim(i);
                    restore_interrupts(pie);
                    return i;
                }
            }
            
         }
    else {
        child = thrtab[tid]; // this will get the child thread from the thread
        //check if the child exit is not childern caller which will give an eror
        if(child->parent != TP){
            return -EINVAL;
        }
       
        while (child->state != THREAD_EXITED) //this will wait on the specific child to exist
        {
            condition_wait(&(TP->child_exit));
        }
        thread_reclaim(child->id); //this will reclaim the resource of the existed childern
        return tid;
    }
    restore_interrupts(pie);
    return tid;
}


const char * thread_name(int tid) {
    assert (0 <= tid && tid < NTHR);
    assert (thrtab[tid] != NULL);
    return thrtab[tid]->name;
}


const char * running_thread_name(void) {
    return TP->name;
}


void condition_init(struct condition * cond, const char * name) {
    tlclear(&cond->wait_list);
    cond->name = name;
}


void condition_wait(struct condition * cond) {
    int pie;


    trace("%s(cond=<%s>) in <%s:%d>", __func__,
        cond->name, TP->name, TP->id);


    assert(TP->state == THREAD_SELF);


    // Insert current thread into cdwait list
   
    set_thread_state(TP, THREAD_WAITING);
    TP->wait_cond = cond;
    TP->list_next = NULL;


    pie = disable_interrupts();
    tlinsert(&cond->wait_list, TP);
    restore_interrupts(pie);


    running_thread_suspend();
}
// Inputs: cond- this will point to the condition variable
// Outputs: None
// Description/Side Effects: This will wake up all the treads waiting on the speficied condtion by moving the
// condition wait of the wait list to the ready queues. The side is thread states
// and will update the scheduling on the queue.
void condition_broadcast(struct condition * cond) {
    // FIXME your code goes here
    struct thread *thr_broadcast;
    int pie;
    pie = disable_interrupts();
    while ((thr_broadcast = tlremove(&cond->wait_list)) != NULL) // check if the all the threads is in the condtion while it does wait list
    {
        set_thread_state(thr_broadcast, THREAD_READY); // thsi will set the thread states to be read which will be schedule
        tlinsert(&ready_list, thr_broadcast); //this will insert the thread into the ready queue
    }
    restore_interrupts(pie);
}


// INTERNAL FUNCTION DEFINITIONS
//


void init_main_thread(void) {
    // Initialize stack anchor with pointer to self
    main_thread.stack_anchor->ktp = &main_thread;
}


void init_idle_thread(void) {
    // Initialize stack anchor with pointer to self
    idle_thread.stack_anchor->ktp = &idle_thread;
}


static void set_running_thread(struct thread * thr) {
    asm inline ("mv tp, %0" :: "r"(thr) : "tp");
}


const char * thread_state_name(enum thread_state state) {
    static const char * const names[] = {
        [THREAD_UNINITIALIZED] = "UNINITIALIZED",
        [THREAD_WAITING] = "WAITING",
        [THREAD_SELF] = "SELF",
        [THREAD_READY] = "READY",
        [THREAD_EXITED] = "EXITED"
    };


    if (0 <= (int)state && (int)state < sizeof(names)/sizeof(names[0]))
        return names[state];
    else
        return "UNDEFINED";
};


void thread_reclaim(int tid) {
    struct thread * const thr = thrtab[tid];
    int ctid;


    assert (0 < tid && tid < NTHR && thr != NULL);
    assert (thr->state == THREAD_EXITED);


    // Make our parent thread the parent of our child threads. We need to scan
    // all threads to find our children. We could keep a list of all of a
    // thread's children to make this operation more efficient.


    for (ctid = 1; ctid < NTHR; ctid++) {
        if (thrtab[ctid] != NULL && thrtab[ctid]->parent == thr)
            thrtab[ctid]->parent = thr->parent;
    }


    thrtab[tid] = NULL;
    kfree(thr);
}


struct thread * create_thread(const char * name) {
    struct thread_stack_anchor * anchor;
    void * stack_page;
    struct thread * thr;
    int tid;


    trace("%s(name=\"%s\") in <%s:%d>", __func__, name, TP->name, TP->id);


    // Find a free thread slot.


    tid = 0;
    while (++tid < NTHR)
        if (thrtab[tid] == NULL)
            break;
   
    if (tid == NTHR)
        return NULL;
   
    // Allocate a struct thread and a stack


    thr = kcalloc(1, sizeof(struct thread));
   
    stack_page = kmalloc(STACK_SIZE);
    anchor = stack_page + STACK_SIZE;
    anchor -= 1; // anchor is at base of stack
    thr->stack_lowest = stack_page;
    thr->stack_anchor = anchor;
    anchor->ktp = thr;
    anchor->kgp = NULL;


    thrtab[tid] = thr;


    thr->id = tid;
    thr->name = name;
    thr->parent = TP;
    return thr;
}
// Inputs: None
// Outputs: None
// Description/Side Effects: The suspend is currently running on the thread and switches to the next availble thread.
// If the thread is waiting, it will switches to the next ready thread. if the thread is running itself, then it set the states to the ready.
//If the thread is exited, it swithces to the next thread and free it resources.
//The side effect is the thread states,ready list,and the context switch.
void running_thread_suspend(void) {
    // FIXME your code goes here
    struct thread *next_thread;
    int pie = disable_interrupts();
    if(TP->state == THREAD_WAITING){ //check if the thread is waiting
        next_thread = tlremove(&ready_list); // this will retrive the next avaible thread from the ready
        set_thread_state(next_thread, THREAD_SELF); // thsi will set the next thread as the current thread
        enable_interrupts(); /// this will enable the interrupts
        _thread_swtch(next_thread); // this will do the context switch
    }
    else if (TP->state == THREAD_SELF) //check if the current thread is running itself
    {
        // this will make the current thread as the readt and place in the ready list
        set_thread_state(TP, THREAD_READY);
        tlinsert(&ready_list, TP);


        //this will retrive the next thread to run from the ready
        next_thread = tlremove(&ready_list);
        set_thread_state(next_thread, THREAD_SELF);


        enable_interrupts(); // this will enable the interrupt
        _thread_swtch(next_thread);
    }
    else if(TP->state == THREAD_EXITED)//check if current thread have an exted states
    {
        //thsi will retrive the next thread to run from the ready
        next_thread = tlremove(&ready_list);
        set_thread_state(next_thread, THREAD_SELF);


        enable_interrupts(); // this will enable the interrupt
        _thread_swtch(next_thread); // this will do the context switch
        kfree(TP->stack_lowest);
    }
    restore_interrupts(pie); // this will enable the interrupt


}


void tlclear(struct thread_list * list) {
    list->head = NULL;
    list->tail = NULL;
}


int tlempty(const struct thread_list * list) {
    return (list->head == NULL);
}


void tlinsert(struct thread_list * list, struct thread * thr) {
    thr->list_next = NULL;


    if (thr == NULL)
        return;


    if (list->tail != NULL) {
        assert (list->head != NULL);
        list->tail->list_next = thr;
    } else {
        assert(list->head == NULL);
        list->head = thr;
    }


    list->tail = thr;
}


struct thread * tlremove(struct thread_list * list) {
    struct thread * thr;


    thr = list->head;
   
    if (thr == NULL)
        return NULL;


    list->head = thr->list_next;
   
    if (list->head != NULL)
        thr->list_next = NULL;
    else
        list->tail = NULL;


    thr->list_next = NULL;
    return thr;
}


// Appends elements of l1 to the end of l0 and clears l1.


// void tlappend(struct thread_list * l0, struct thread_list * l1) {
//     if (l0->head != NULL) {
//         assert(l0->tail != NULL);
       
//         if (l1->head != NULL) {
//             assert(l1->tail != NULL);
//             l0->tail->list_next = l1->head;
//             l0->tail = l1->tail;
//         }
//     } else {
//         assert(l0->tail == NULL);
//         l0->head = l1->head;
//         l0->tail = l1->tail;
//     }


//     l1->head = NULL;
//     l1->tail = NULL;
// }


void idle_thread_func(void) {
    // The idle thread sleeps using wfi if the ready list is empty. Note that we
    // need to disable interrupts before checking if the thread list is empty to
    // avoid a race condition where an ISR marks a thread ready to run between
    // the call to tlempty() and the wfi instruction.


    for (;;) {
        // If there are runnable threads, yield to them.


        while (!tlempty(&ready_list))
            thread_yield();
       
        // No runnable threads. Sleep using the wfi instruction. Note that we
        // need to disable interrupts and check the runnable thread list one
        // more time (make sure it is empty) to avoid a race condition where an
        // ISR marks a thread ready before we call the wfi instruction.


        disable_interrupts();
        if (tlempty(&ready_list))
            asm ("wfi");
        enable_interrupts();
    }
}

// Inputs: lock - pointer to the lock to initialize
// Outputs: None
// Description: Initializes the lock struct, setting the owner and next lock to NULL, count to 0,
// and linking the condition variable used to block waiting threads.
void lock_init(struct lock * lock) {
    // initializing lock parameters
    lock->owner = NULL;
    lock->count = 0;
    lock->next = NULL;
    condition_init(&lock->lock_release, "lock_cond");
}

void lock_acquire(struct lock * lock) {
    // disabling interrupts before modifying lock_list
    int pie = disable_interrupts();

    // if the lock's owner is the currently running thread, increment the count, restore interrupts and return
    if (lock->owner == TP) {
        lock->count++;
        restore_interrupts(pie);
        return;
    }

    // waiting until lock is released
    while (lock->owner != NULL) {
        condition_wait(&lock->lock_release);
    }

    // acquiring the lock
    lock->owner = TP;
    lock->count = 1;

    // adding lock to thread's lock_list
    lock->next = TP->lock_list;
    TP->lock_list = lock;

    restore_interrupts(pie);
}

void lock_release(struct lock * lock) {
    // disabling interrupts before modifying lock_list
    int pie = disable_interrupts();

    // making sure the currently running thread is the lock's owner
    assert(lock->owner == TP);

    // if the lock's access count is greater than one, decrement count, restore interrupts and return
    if (lock->count > 1) {
        lock->count--;
        restore_interrupts(pie);
        return;
    }

    // releasing the lock
    lock->owner = NULL;
    lock->count = 0;

    struct lock *prev = NULL;
    struct lock *curr = TP->lock_list;

    // move through the lock list to find the current lock
    while (curr != NULL) {
        if (curr == lock) {
            // if lock is at the head of the list
            if (prev == NULL) {
                TP->lock_list = curr->next;
            }
            // if the lock is after the head
            else {
                prev->next = curr->next;
            }
            break; // found the lock, exit
        }
        // move to next lock in lock_list
        prev = curr;
        curr = curr->next;
    }

    // clearing the lock's next parameter
    lock->next = NULL;

    // waking up any threads waiting for this lock
    condition_broadcast(&lock->lock_release);

    restore_interrupts(pie);
}