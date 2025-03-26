// plic.c - RISC-V PLIC
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef PLIC_TRACE
#define TRACE
#endif

#ifdef PLIC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "plic.h"
#include "assert.h"

#include <stdint.h>

// INTERNAL MACRO DEFINITIONS
//

// CTX(i,0) is hartid /i/ M-mode context
// CTX(i,1) is hartid /i/ S-mode context

#define CTX(i,s) (2*(i)+(s))

// INTERNAL TYPE DEFINITIONS
// 

struct plic_regs {
	union {
		uint32_t priority[PLIC_SRC_CNT];
		char _reserved_priority[0x1000];
	};

	union {
		uint32_t pending[PLIC_SRC_CNT/32];
		char _reserved_pending[0x1000];
	};

	union {
		uint32_t enable[PLIC_CTX_CNT][32];
		char _reserved_enable[0x200000-0x2000];
	};

	struct {
		union {
			struct {
				uint32_t threshold;
				uint32_t claim;
			};
			
			char _reserved_ctxctl[0x1000];
		};
	} ctx[PLIC_CTX_CNT];
};

#define PLIC (*(volatile struct plic_regs*)PLIC_MMIO_BASE)

// INTERNAL FUNCTION DECLARATIONS
//

static void plic_set_source_priority (
	uint_fast32_t srcno, uint_fast32_t level);
static int plic_source_pending(uint_fast32_t srcno);
static void plic_enable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);
static void plic_disable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);
static void plic_set_context_threshold (
	uint_fast32_t ctxno, uint_fast32_t level);
static uint_fast32_t plic_claim_context_interrupt (
	uint_fast32_t ctxno);
static void plic_complete_context_interrupt (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_enable_all_sources_for_context(uint_fast32_t ctxno);
static void plic_disable_all_sources_for_context(uint_fast32_t ctxno);

// We currently only support single-hart operation, sending interrupts to S mode
// on hart 0 (context 0). The low-level PLIC functions already understand
// contexts, so we only need to modify the high-level functions (plit_init,
// plic_claim_request, plic_finish_request)to add support for multiple harts.

// EXPORTED FUNCTION DEFINITIONS
// 

void plic_init(void) {
	int i;

	// Disable all sources by setting priority to 0

	for (i = 0; i < PLIC_SRC_CNT; i++)
		plic_set_source_priority(i, 0);
	
	// Route all sources to S mode on hart 0 only

	for (int i = 0; i < PLIC_CTX_CNT; i++)
		plic_disable_all_sources_for_context(i);
	
	plic_enable_all_sources_for_context(CTX(0,1));
}

extern void plic_enable_source(int srcno, int prio) {
	trace("%s(srcno=%d,prio=%d)", __func__, srcno, prio);
	assert (0 < srcno && srcno <= PLIC_SRC_CNT);
	assert (prio > 0);

	plic_set_source_priority(srcno, prio);
}

extern void plic_disable_source(int irqno) {
	if (0 < irqno)
		plic_set_source_priority(irqno, 0);
	else
		debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_interrupt(void) {
	// FIXME: Hardwired S-mode hart 0
	trace("%s()", __func__);
	return plic_claim_context_interrupt(CTX(0,1));
}

extern void plic_finish_interrupt(int irqno) {
	// FIXME: Hardwired S-mode hart 0
	trace("%s(irqno=%d)", __func__, irqno);
	plic_complete_context_interrupt(CTX(0,1), irqno);
}

// INTERNAL FUNCTION DEFINITIONS
//
//Inputs:
//uint_fast32_t srcno - This is the interrupt source number 
//uint_fast32_t level - This is the priority level of the interrupt source
//Outputs: None
//Description/Side effect: This function sets the priority level for a given interrupt source
//after checking the srcno is within the valid range.
// If the source number is invalid, the function exits to avoid interrupt source. 
// The side effect is priority regsiter for the specificed sources that I changes
static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level) {
	// FIXME your code goes here

	if (srcno >= PLIC_SRC_CNT) //check if the srcno number is within the valid ranges to avoid invild memory 
	{ 
	return; // exit to make sure there is no interrupt source 
	}
	PLIC.priority[srcno]=level; // set the priority level of the given interrupt sources
}

//Inputs:
//uint_fast32_t srcno - This is the interrupt source number 
//Outputs: It retrun the non zero of the interrupt source which is pending 
//Description:This function checks if an interrupt source is currently pending in the PLIC by ensuring the srcno is valid 
//and then perform the bitwise operation to determine if the corresponding bit is set, returning a non-zero value if it is pending. 
//side effect: None 
static inline int plic_source_pending(uint_fast32_t srcno) {
	// FIXME your code goes here
	if (srcno >= PLIC_SRC_CNT) //check if the srcno number is within the valid ranges to avoid invild memory 
	{
	return 0;   // exit return 0 to indicate no interrupt sources 
	}
	return (PLIC.pending[srcno / 32] & (1 << (srcno % 32))) !=0; // doing bitwise and check if the set in the pending array, return non zero if it pending 
}
//Inputs:
//uint_fast32_t ctxno - The context number is with the interrupt source of the enable.
//uint_fast32_t srcno - This is the interrupt source number of the enable 
// Outputs: None
// Description/Side Effects:This function enables a specific interrupt srcno for a given ctxno in the PLIC.
// It then sets the corresponding bit in the enable array to allow the interrupt.
// The side effect will the update the interrupt enable state of the given context for the register 
static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno) 
{
	// FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT || srcno >= PLIC_SRC_CNT) // check if ctxno and srcno are vaild before enabling interrupt 
	{
		return; //exit the return to invalid values 
	}
	PLIC.enable[ctxno][srcno / 32] = PLIC.enable[ctxno][srcno / 32] | (1U << (srcno % 32)); /// set the bit in the enable array to allow to interrupts in order to set the bit
}

// Inputs: 
//uint_fast32_t ctxno - The context number is with the interrupt source of the disable.
//uint_fast32_t srcid - This is the interrupt source number of the disable  
// Outputs: None
// Description/Side Effects: This function disables a specific interrupt source for a given context in the PLIC by checking the input values  
// and then clearing the corresponding bit in the array using bitwise AND with the inverse of the bit mask.
// The side effect will update the interrupt enable state of the given context for the register. 
static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid) 
{
	// FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT || srcid >= PLIC_SRC_CNT) //check if ctxno and srcno to prevent error 
	{
	return; //exit the return to invalid values 
	}
	PLIC.enable[ctxno][srcid / 32] = PLIC.enable[ctxno][srcid / 32] & ~(1U << (srcid % 32));  //clear the bit in the enable array using bitwise and inserve mark
}

// Inputs:
//uint_fast32_t ctxno -  The context number is with the thrsehold of the context
//uint_fast32_t level -  This is the priority of the theshold of the context 
//Outputs: None
//Description/Side Effects: This function sets the priority threshold for a given context in the PLIC by checking that the ctxno is valid
// and then assigning the specified level to the context threshold.
//  The side effect will affect the handle of the interrupts of the thresehold register 
static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level)
{
	// FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT) //check if the ctxno is valid before setting the threshold
	{
	return; //exit the return to invalid values 
	}
	PLIC.ctx[ctxno].threshold = level;	// set the priority level of the given interrupt sources
}

// Inputs:
//  uint_fast32_t ctxno - The context number is with the claim to the interrupt
// Outputs: this will high priority pending interrput for the context. 
// Description/Side Effect:This function returns the highest-priority interrupt for ctxno in the PLIC by checking the context number.
// It also accesses the claim register for the specified context.
// The side effect will remvoe the interrupt from the pending. 
static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno) {
	// FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT)  //check if the ctxno is valid before claiming the interrupt
	{
	return 0;// exit the return 0 to ctxno invalid
	}
	return PLIC.ctx[ctxno].claim; ///return the highest priorpity pending interrupt of the ctxno
}


// Inputs: 
// uint_fast32_t ctxno - The context number of the interrupt is cmpleted 
// uint_fast32_t srcno -The interrupt source number for the completed 
// Outputs: None
// Description/Side Effects: This function completes an interrupt for a given context in the PLIC by checking the context and source numbers.
// It also writes the srcno to the claim register to mark the interrupt as completed.
// This side effect is make sure that the interrupt is completed. 
static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno) {
	// FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT || srcno >= PLIC_SRC_CNT) // check if the ctxno and srcno before completing the interrupt 
	{
	return; //exit the return to invalid values 
	}
	PLIC.ctx[ctxno].claim = srcno; //write the srcno to the claim register to mark the interrupt and complete the interrupt 
}
// Inputs: 
// uint_fast32_t ctxno - The context number of the interrupt is enable
// Outputs: None
// Description/Side Effects: This function enables all interrupt sources by checking the ctxno number.
// It also loops through each source to set the corresponding bit in the enable array.
// The side effect is that it will enable the interrupt source of the context. 
static void plic_enable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT) // check if the ctxno before enabling all sources  
	{
	return; //exit the return to invalid values 
	}
	for(uint_fast32_t i = 0; i < 32; i++) //check all sources and enable to setting bits by looping through 
	{
		PLIC.enable[ctxno][i / 32] = PLIC.enable[ctxno][i / 32] | (1U << (i % 32)); //setting each bit
	}
}

// Inputs:
// uint_fast32_t ctxno - The context number of the interrupt is disable
// Outputs: none 
// Description/Side Effects: This function disables all interrupt sources for the given context by checking the context number.
// It loops through each source to clear all bits in the enable array, effectively disabling the interrupts.
// The side effect is that it will disable the interrupt source of the context. 

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
	if (ctxno >= PLIC_CTX_CNT) // check if the ctxno before disabling all sources  
	{
	 return; //exit the return to invalid values 
	}
	for(uint_fast32_t i = 0; i < 32; i++) //check all sources and disable them by looping through 
	{
        PLIC.enable[ctxno][i / 32] = 0;  ///clear all but to disable the sources 
	}
}
