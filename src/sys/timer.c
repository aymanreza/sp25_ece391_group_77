// timer.c
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//
#ifdef TIMER_TRACE
#define TRACE
#endif


#ifdef TIMER_DEBUG
#define DEBUG
#endif


#include "timer.h"
#include "thread.h"
#include "riscv.h"
#include "assert.h"
#include "intr.h"
#include "conf.h"
#include "see.h" // for set_stcmp


// EXPORTED GLOBAL VARIABLE DEFINITIONS
// 


char timer_initialized = 0;


// INTERNVAL GLOBAL VARIABLE DEFINITIONS
//


static struct alarm * sleep_list;


// INTERNAL FUNCTION DECLARATIONS
//


// EXPORTED FUNCTION DEFINITIONS
//


void timer_init(void) {
    set_stcmp(UINT64_MAX);
    timer_initialized = 1;
}


// Inputs: al- pointer to the alarm
//  name- name of the alarm
// Outputs: none
// Description/Side Effects: It will intalize the alram by setting the condtion name,
// initialize the condtion variable, setting the wake up time for the current time and
// make sure it is not linked to any other alarm. The side efffect is the alarm structure.
void alarm_init(struct alarm * al, const char * name) {
    // FIXME your code goes here
    if (name == NULL) { // check if it null and give the name "alarm"
        name = "alarm";
    }
    al->cond.name = name; // this will give the alram condtion name
    condition_init(&al->cond, name); // ths will initalize the condition for this alarm
    al->twake = rdtime(); //this will wake time to the current time
    al->next = NULL; // this will ensure that alram is not linked to other alram
   
}


// Inputs: al- pointer to the alarm
// tcnt- This is the time count after the alarm should be trigger
// Outputs: None
// Description/Side Effects: The schedule of the alarm in the sleep list based on the wake up time.
//If the alram doesn't exist, it set the system timer to the next wake up time.
//The side effect is the sleep list, update the system timer, and enable the timer interrupt.
void alarm_sleep(struct alarm * al, unsigned long long tcnt) {
    unsigned long long now;
    struct alarm * prev;
    int pie;


    now = rdtime();


    // If the tcnt is so large it wraps around, set it to UINT64_MAX


    if (UINT64_MAX - al->twake < tcnt)
        al->twake = UINT64_MAX;
    else
        al->twake += tcnt;
   
    // If the wake-up time has already passed, return


    if (al->twake < now)
        return;


    // FIXME your code goes here


 
    pie = disable_interrupts();
    if (sleep_list == NULL) { //check if al is at the head
        al->next = NULL;
        sleep_list = al; //this will set the sleep list head
        set_stcmp(al->twake); // this will set to wake up next time
       
    }
    else if (al->twake < sleep_list->twake) { // check if insert at the head
        al->next = sleep_list;  //the alram will point to the old head
        sleep_list = al; // this will update the pointer


        set_stcmp(al->twake); //this will set the system time for the wake up time
    }
   
    else { // this will check middle or at the end of the list
        prev = NULL;
        struct alarm *iter = sleep_list;
        while (iter && iter->twake < al->twake) { //this will find the correct insertion of the point in the stored list
            prev = iter;
            iter = iter->next;
        }


        //this will insert the alram
        prev->next = al;
        al->next = iter;
    }
   
    csrs_sie(RISCV_SCAUSE_STI); // thsi will enable the timer interrupt
   
   
   
    condition_wait(&al->cond); // thsi will  condtion wait until the arlam condtion is signal
   
    restore_interrupts(pie); // this will restore the interrupt
   
}


// Resets the alarm so that the next sleep increment is relative to the time
// alarm_reset is called.


void alarm_reset(struct alarm * al) {
    al->twake = rdtime();
}


void alarm_sleep_sec(struct alarm * al, unsigned int sec) {
    alarm_sleep(al, sec * TIMER_FREQ);
}


void alarm_sleep_ms(struct alarm * al, unsigned long ms) {
    alarm_sleep(al, ms * (TIMER_FREQ / 1000));
}


void alarm_sleep_us(struct alarm * al, unsigned long us) {
    alarm_sleep(al, us * (TIMER_FREQ / 1000 / 1000));
}


void sleep_sec(unsigned int sec) {
    sleep_ms(1000UL * sec);
}


void sleep_ms(unsigned long ms) {
    sleep_us(1000UL * ms);
}


void sleep_us(unsigned long us) {
    struct alarm al;


    alarm_init(&al, "sleep");
    alarm_sleep_us(&al, us);
}




// Inputs: None
// Outputs: None
// Description/Side Effects: This will handle the timer interrupt by waking up the threads
//where the alarms have expiced and setting the next waake up time.It will diable the time
//if there is no alarm remain. The side effect is the sleep list, wake up threads and disbale or updating the timer.
void handle_timer_interrupt(void) {
    struct alarm * head = sleep_list;
    struct alarm * next;
    uint64_t now;


    now = rdtime();


    trace("[%lu] %s()", now, __func__);
    debug("[%lu] mtcmp = %lu", now, rdtime());


    // FIXME your code goes here
   
    int pie = disable_interrupts();  // this will disable the interrupt
    while (head && head->twake <= now) // check though the sleep list and processes that need to triggered
    {
        next = head->next; // this will store the next alarm
        condition_broadcast(&head->cond); // This will wake up the threads waiting on the arlam
        head->next = NULL; // ths will clear the pointer
        head = next;  // this will mvoe to the next arlam
    }
    sleep_list = head; //this will update the sleep list to the new alram
    if (sleep_list) // this will check the next wake up event if there still more alram
    {
        set_stcmp(sleep_list->twake); // this will wake up the next alram time
    }
    else
    {
        csrc_sie(RISCV_SCAUSE_STI); //this disable the timer interrupt
        set_stcmp(UINT64_MAX);  // this will set the time to the max
    }


    restore_interrupts(pie); //this will restore the interrupt
}
