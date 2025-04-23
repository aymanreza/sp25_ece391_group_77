/*! @file memory.c
    @brief Physical and virtual memory manager    
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#ifdef MEMORY_TRACE
#define TRACE
#endif

#ifdef MEMORY_DEBUG
#define DEBUG
#endif

#include "memory.h"
#include "conf.h"
#include "riscv.h"
#include "heap.h"
#include "console.h"
#include "assert.h"
#include "string.h"
#include "thread.h"
#include "process.h"
#include "error.h"

// COMPILE-TIME CONFIGURATION
//

// Minimum amount of memory in the initial heap block.

#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif

// INTERNAL CONSTANT DEFINITIONS
//

#define MEGA_SIZE ((1UL << 9) * PAGE_SIZE) // megapage size
#define GIGA_SIZE ((1UL << 9) * MEGA_SIZE) // gigapage size

#define PTE_ORDER 3
#define PTE_CNT (1U << (PAGE_ORDER - PTE_ORDER))

#ifndef PAGING_MODE
#define PAGING_MODE RISCV_SATP_MODE_Sv39
#endif

#ifndef ROOT_LEVEL
#define ROOT_LEVEL 2
#endif

// IMPORTED GLOBAL SYMBOLS
//

// linker-provided (kernel.ld)
extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];

// EXPORTED GLOBAL VARIABLES
//

char memory_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

// We keep free physical pages in a linked list of _chunks_, where each chunk
// consists of several consecutive pages of memory. Initially, all free pages
// are in a single large chunk. To allocate a block of pages, we break up the
// smallest chunk on the list.

/**
 * @brief Section of consecutive physical pages. We keep free physical pages in a
 * linked list of chunks. Initially, all free pages are in a single large chunk. To
 * allocate a block of pages, we break up the smallest chunk in the list
 */
struct page_chunk {
    struct page_chunk * next; ///< Next page in list
    unsigned long pagecnt; ///< Number of pages in chunk
};

/**
 * @brief RISC-V PTE. RTDC (RISC-V docs) for what each of these fields means!
 */
struct pte {
    uint64_t flags:8;
    uint64_t rsw:2;
    uint64_t ppn:44;
    uint64_t reserved:7;
    uint64_t pbmt:2;
    uint64_t n:1;
};

// INTERNAL MACRO DEFINITIONS
//

#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2*9)) % PTE_CNT)
#define VPN1(vma) ((VPN(vma) >> (1*9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0*9)) % PTE_CNT)

#define MIN(a,b) (((a)<(b))?(a):(b))

#define ROUND_UP(n,k) (((n)+(k)-1)/(k)*(k)) 
#define ROUND_DOWN(n,k) ((n)/(k)*(k))

// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).

#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)

#define PT_INDEX(lvl, vpn) (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) \
                             >> (lvl * (PAGE_ORDER - PTE_ORDER)))
// INTERNAL FUNCTION DECLARATIONS
//



static inline mtag_t active_space_mtag(void);
static inline mtag_t ptab_to_mtag(struct pte * root, unsigned int asid);
static inline struct pte * mtag_to_ptab(mtag_t mtag);
static inline struct pte * active_space_ptab(void);

static inline void * pageptr(uintptr_t n);
static inline uintptr_t pagenum(const void * p);
static inline int wellformed(uintptr_t vma);

static inline struct pte leaf_pte(const void * pp, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte(const struct pte * pt, uint_fast8_t g_flag);
static inline struct pte null_pte(void);

// INTERNAL GLOBAL VARIABLES
//

static mtag_t main_mtag;

static struct pte main_pt2[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt1_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__ ((section(".bss.pagetable"), aligned(4096)));

static struct page_chunk * free_chunk_list;

// EXPORTED FUNCTION DECLARATIONS
// 

void memory_init(void) {
    const void * const text_start = _kimg_text_start;
    const void * const text_end = _kimg_text_end;
    const void * const rodata_start = _kimg_rodata_start;
    const void * const rodata_end = _kimg_rodata_end;
    const void * const data_start = _kimg_data_start;
    
    void * heap_start;
    void * heap_end;

    uintptr_t pma;
    const void * pp;

    trace("%s()", __func__);

    assert (RAM_START == _kimg_start);

    kprintf("           RAM: [%p,%p): %zu MB\n",
        RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    kprintf("  Kernel image: [%p,%p)\n", _kimg_start, _kimg_end);

    // Kernel must fit inside 2MB megapage (one level 1 PTE)
    
    if (MEGA_SIZE < _kimg_end - _kimg_start)
        panic(NULL);

    // Initialize main page table with the following direct mapping:
    // 
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB
    
    // Identity mapping of MMIO region as two gigapage mappings
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)
        main_pt2[VPN2(pma)] = leaf_pte((void*)pma, PTE_R | PTE_W | PTE_G);
    
    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.

    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_G);
    }

    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages

    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] =
            leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Enable paging; this part always makes me nervous.

    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = (void*)ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);

    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += ROUND_UP (
            HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end)
        panic("out of memory");
    
    // Initialize heap memory manager

    heap_init(heap_start, heap_end);

    kprintf("Heap allocator: [%p,%p): %zu KB free\n",
        heap_start, heap_end, (heap_end - heap_start) / 1024);
    
    // TODO: Initialize the free chunk list here
    // finding # of free pages by dividing total available memory by page size
    unsigned long free_pages = (unsigned long)(RAM_END - (uintptr_t)heap_end) / PAGE_SIZE;
    // casting start of available memory(heap end) as page chunk and initializing free_chunk_list
    free_chunk_list = (struct page_chunk *)heap_end;
    free_chunk_list->pagecnt = free_pages;
    free_chunk_list->next = NULL;
    
    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).

    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
}

mtag_t active_mspace(void) {
    return active_space_mtag();
}

mtag_t switch_mspace(mtag_t mtag) {
    mtag_t prev;
    
    prev = csrrw_satp(mtag);
    sfence_vma();
    return prev;
}

mtag_t clone_active_mspace(void) {
    // mtag_t TODO; 
    // TODO = 0; 
    return main_mtag; // for cp2 single memory space, simply return the current active memory space
}

void reset_active_mspace(void) {
    // iterating over all pages in the user memory
    for (uintptr_t address = UMEM_START_VMA; address < UMEM_END_VMA; address += PAGE_SIZE) {
        // getting pointer to level 2 page table
        struct pte *lvl2 = active_space_ptab();
        // using macro to compute index into level 2 page table
        unsigned int lvl2_idx = VPN2(address);
        if (!PTE_VALID(lvl2[lvl2_idx])) continue;

        // finding address of level 1 page table by left shifting by PAGE_ORDER
        struct pte *lvl1 = (struct pte *)((uintptr_t)lvl2[lvl2_idx].ppn << PAGE_ORDER);
        // using macro to compute level 1 index
        unsigned int lvl1_idx = VPN1(address);
        if (!PTE_VALID(lvl1[lvl1_idx])) continue;

        // finding address of level 0 page table by left shifting by PAGE_ORDER
        struct pte *lvl0 = (struct pte *)((uintptr_t)lvl1[lvl1_idx].ppn << PAGE_ORDER);
        // using macro to compute level 0 index
        unsigned int lvl0_idx = VPN0(address);
        if (!PTE_VALID(lvl0[lvl0_idx])) continue;

        // checking that lvl0 entry is valid
        if (PTE_VALID(lvl0[lvl0_idx])) {
            // getting current physical page number
            uintptr_t ppn = lvl0[lvl0_idx].ppn;
            kprintf("lvl0[lvl0_idx].ppn\n");

            // converting page number to a pointer to the physical address of the page
            void *phys_page_addr = (void *)((uintptr_t)ppn << PAGE_ORDER);

            // freeing the physical page
            free_phys_page(phys_page_addr);

            // clearing the PTE
            lvl0[lvl0_idx] = null_pte();
        }   
    }
    // flushing the TLB to ensure unmapped pages aren't in cache
    sfence_vma(); 
}

mtag_t discard_active_mspace(void) {
    // mtag_t TODO; 
    // TODO = 0;
    // // resetting and returning the current active memory space
    // reset_active_mspace();
    return main_mtag; 
}

// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.

// We currently map 4K pages only. At some point it may be disirable to support
// mapping megapages and gigapages.

void * map_page(uintptr_t vma, void * pp, int rwxug_flags) {
    // checking that page is properly aligned
    assert(vma % PAGE_SIZE == 0);

    // checking that page is well formed
    assert(wellformed(vma));

    // getting pointer to level 2 page table
    struct pte *lvl2 = active_space_ptab();

    // using macro to compute index into level 2 page table
    unsigned int lvl2_idx = VPN2(vma);

    // checking that lvl2 entry is valid
    if (!PTE_VALID(lvl2[lvl2_idx])) {
        // if not, allocate new page for lvl1 table, clear it, 
        // and create a new PTE for it to assign to the lvl2 entry
        void *new_lvl1 = alloc_phys_page();
        memset(new_lvl1, 0, PAGE_SIZE);
        lvl2[lvl2_idx] = ptab_pte((struct pte *)new_lvl1, PTE_G);
    }

    // finding address of level 1 page table by left shifting by PAGE_ORDER
    struct pte *lvl1 = (struct pte *)((uintptr_t)lvl2[lvl2_idx].ppn << PAGE_ORDER);

    // using macro to compute level 1 index
    unsigned int lvl1_idx = VPN1(vma);

    // checking that lvl1 entry is valid
    if (!PTE_VALID(lvl1[lvl1_idx])) {
        // if not, allocate new page for lvl0 table, clear it, 
        // and create a new PTE for it to assign to the lvl0 entry
        void *new_lvl0 = alloc_phys_page();
        memset(new_lvl0, 0, PAGE_SIZE);
        lvl1[lvl1_idx] = ptab_pte((struct pte *)new_lvl0, PTE_G);
    }

    // finding address of level 0 page table by left shifting by PAGE_ORDER
    struct pte *lvl0 = (struct pte *)((uintptr_t)lvl1[lvl1_idx].ppn << PAGE_ORDER);

    // using macro to compute level 0 index
    unsigned int lvl0_idx = VPN0(vma);

    // kprintf("Mapping vaddr 0x%lx to physical %p (VPN2=%u, VPN1=%u, VPN0=%u)\n", vma, pp, VPN2(vma), VPN1(vma), VPN0(vma));

    // setting leaf PTE
    lvl0[lvl0_idx] = leaf_pte(pp, rwxug_flags);

    // struct pte p = lvl0[lvl0_idx];
    // kprintf("Final PTE: flags=0x%x, ppn=0x%lx\n", p.flags, p.ppn);

    // flushing the TLB
    sfence_vma();

    return (void *)vma;
}

void * map_range(uintptr_t vma, size_t size, void * pp, int rwxug_flags) {
    // ensuring that the size is a multiple of PAGE_SIZE
    size_t rounded_size = ROUND_UP(size, PAGE_SIZE);

    // iterating over all pages in range
    for (size_t page_offset = 0; page_offset < rounded_size; page_offset += PAGE_SIZE) {
        // calculating page virtual and physical addresses and mapping each page
        uintptr_t page_vma = vma + page_offset;
        void * page_pp = (void *)((uintptr_t)pp + page_offset);
        map_page(page_vma, page_pp, rwxug_flags);
    }

    return (void *)vma; 
}

void * alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
    // making sure size is a multiple of PAGE_SIZE and calculating page count
    unsigned int page_count = ROUND_UP(size, PAGE_SIZE) / PAGE_SIZE;

    // allocating pages and storing returned physical address pointer
    void* phys_address_pointer = alloc_phys_pages(page_count);
    if (!phys_address_pointer) panic("Failed to alloc physical pages!");

    // kprintf("Allocated %u page(s) at physical %p for vaddr 0x%lx\n", page_count, phys_address_pointer, vma);

    // mapping pages using returned physical address pointer
    map_range(vma, size, phys_address_pointer, rwxug_flags);

    return (void *)vma; 
}

void set_range_flags(const void * vp, size_t size, int rwxug_flags) {
    // ensuring that the size is a multiple of PAGE_SIZE
    size_t rounded_size = ROUND_UP(size, PAGE_SIZE);

    // iterating over all pages in range
    for (size_t page_offset = 0; page_offset < rounded_size; page_offset += PAGE_SIZE) {
        // calculating page virtual and physical addresses and mapping each page
        uintptr_t page_vma = (uintptr_t)vp + page_offset;

        // getting pointer to level 2 page table
        struct pte *lvl2 = active_space_ptab();
        // using macro to compute index into level 2 page table
        unsigned int lvl2_idx = VPN2(page_vma);

        // finding address of level 1 page table by left shifting by PAGE_ORDER
        struct pte *lvl1 = (struct pte *)((uintptr_t)lvl2[lvl2_idx].ppn << PAGE_ORDER);
        // using macro to compute level 1 index
        unsigned int lvl1_idx = VPN1(page_vma);

        // finding address of level 0 page table by left shifting by PAGE_ORDER
        struct pte *lvl0 = (struct pte *)((uintptr_t)lvl1[lvl1_idx].ppn << PAGE_ORDER);
        // using macro to compute level 0 index
        unsigned int lvl0_idx = VPN0(page_vma);

        // getting current physical page number
        uintptr_t ppn = lvl0[lvl0_idx].ppn;

        // creating new leaf PTE with updated flags
        lvl0[lvl0_idx] = leaf_pte(pageptr(ppn), rwxug_flags);
    }

    // flushing TLB after updating range
    sfence_vma(); 
}

void unmap_and_free_range(void * vp, size_t size) {
    // ensuring that the size is a multiple of PAGE_SIZE
    size_t rounded_size = ROUND_UP(size, PAGE_SIZE);

    // iterating over all pages in range
    for (size_t page_offset = 0; page_offset < rounded_size; page_offset += PAGE_SIZE) {
        // calculating page virtual and physical addresses and mapping each page
        uintptr_t page_vma = (uintptr_t)vp + page_offset;

        // getting pointer to level 2 page table
        struct pte *lvl2 = active_space_ptab();
        // using macro to compute index into level 2 page table
        unsigned int lvl2_idx = VPN2(page_vma);

        // finding address of level 1 page table by left shifting by PAGE_ORDER
        struct pte *lvl1 = (struct pte *)((uintptr_t)lvl2[lvl2_idx].ppn << PAGE_ORDER);
        // using macro to compute level 1 index
        unsigned int lvl1_idx = VPN1(page_vma);

        // finding address of level 0 page table by left shifting by PAGE_ORDER
        struct pte *lvl0 = (struct pte *)((uintptr_t)lvl1[lvl1_idx].ppn << PAGE_ORDER);
        // using macro to compute level 0 index
        unsigned int lvl0_idx = VPN0(page_vma);

        // getting current physical page number
        uintptr_t ppn = lvl0[lvl0_idx].ppn;

        // converting page number to a pointer to the physical address of the page
        void *phys_page_addr = (void *)((uintptr_t)ppn << PAGE_ORDER);

        // freeing the physical page
        free_phys_page(phys_page_addr);

        // clearing the PTE
        lvl0[lvl0_idx] = null_pte();
    }

    // flushing TLB after updating range
    sfence_vma();
}

void * alloc_phys_page(void) {
    return alloc_phys_pages(1); // same as multiple pages but with pagecnt of 1
}

void free_phys_page(void * pp) {
    free_phys_pages(pp, 1); // same as multiple pages but with pagecnt of 1
}

void * alloc_phys_pages(unsigned int cnt) {
    // declaring variables for navigating free_chunk_list and holding output
    struct page_chunk *curr = free_chunk_list;
    struct page_chunk *prev = NULL;
    void * phys_address = NULL;

    // iterating through free_chunk_list until I find one with enough pages
    while (curr != NULL) {
        // checking if current chunk has at least my desired page count
        if (curr->pagecnt >= cnt) {
            phys_address = (void *)curr; // storing address of the desired chunk
            // case 1: chunk->pagecnt == cnt
            if (curr->pagecnt == cnt) {
                // if at head of free_chunk_list, move to next node
                if (prev == NULL) {
                    free_chunk_list = curr->next;
                }
                // else update prev next pointer to current next node
                else {
                    prev->next = curr->next;
                }
            }
            // case 2: chunk->pagecnt > cnt
            else {
                // create a new chunk after subtracting cnt pages
                struct page_chunk *new_chunk = (struct page_chunk *)((uintptr_t)curr + cnt * PAGE_SIZE);
                // update new chunk's pagecnt and link to next node in free_chunk_list
                new_chunk->pagecnt = curr->pagecnt - cnt;
                new_chunk->next = curr->next;
                // if at head of free_chunk_list, move to next node
                if (prev == NULL) {
                    free_chunk_list = new_chunk;
                } 
                // else, update prev pointer to the new chunk
                else {
                    prev->next = new_chunk;
                }
            }
            return phys_address;
        }
        // move to next node in free_chunk_list
        prev = curr;
        curr = curr->next;
    }
    // if no chunk is found, panic
    panic("ran out of physical memory for allocating pages");
    return NULL;
}

void free_phys_pages(void * pp, unsigned int cnt) {
    // casting provided page base address to a chunk
    struct page_chunk * free_chunk = (struct page_chunk *)pp;

    // setting provided chunk pagecount
    free_chunk->pagecnt = cnt;

    // adding freed chunk to head of free_chunk_list
    free_chunk->next = free_chunk_list;
    free_chunk_list = free_chunk;
}

unsigned long free_phys_page_count(void) {
    // declaring variables for navigating free_chunk_list and holding the final page count
    struct page_chunk *curr = free_chunk_list;
    unsigned long count = 0;

    // iterating through free_chunk_list and adding each chunk's pagecount to count
    while (curr != NULL) {
        count += curr->pagecnt;
        curr = curr->next;
    }

    return count; //returning total free page count
}

int handle_umode_page_fault(struct trap_frame * tfr, uintptr_t vma) {
    // checking that vma is within user memory
    if (vma < UMEM_START_VMA || vma >= UMEM_END_VMA) {
        // if not, return 0
        return 0;
    }

    vma = ROUND_DOWN(vma, PAGE_SIZE);

    unsigned long long cause = csrr_scause();

    int flags = PTE_R | PTE_U;


    if (cause == RISCV_SCAUSE_STORE_PAGE_FAULT) {
        flags |= PTE_W;
    }

    if (cause == RISCV_SCAUSE_INSTR_PAGE_FAULT) {
        flags |= PTE_X;
    }
    // allocating a new physical page
    void *new_page = alloc_phys_page();
    assert(new_page != NULL);

    // mapping the new page with given vma and user-mode flags
    map_page(vma, new_page, flags);

    return 1; // signaling handled fault
}


mtag_t active_space_mtag(void) {
    return csrr_satp();
}

static inline mtag_t ptab_to_mtag(struct pte * ptab, unsigned int asid) {
    return (
        ((unsigned long)PAGING_MODE << RISCV_SATP_MODE_shift) |
        ((unsigned long)asid << RISCV_SATP_ASID_shift) |
        pagenum(ptab) << RISCV_SATP_PPN_shift);
}

static inline struct pte * mtag_to_ptab(mtag_t mtag) {
    return (struct pte *)((mtag << 20) >> 8);
}

static inline struct pte * active_space_ptab(void) {
    return mtag_to_ptab(active_space_mtag());
}

static inline void * pageptr(uintptr_t n) {
    return (void*)(n << PAGE_ORDER);
}

static inline unsigned long pagenum(const void * p) {
    return (unsigned long)p >> PAGE_ORDER;
}

static inline int wellformed(uintptr_t vma) {
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits+1));
}

static inline struct pte leaf_pte(const void * pp, uint_fast8_t rwxug_flags) {
    return (struct pte) {
        .flags = rwxug_flags | PTE_A | PTE_D | PTE_V,
        .ppn = pagenum(pp)
    };
}

static inline struct pte ptab_pte(const struct pte * pt, uint_fast8_t g_flag) {
    return (struct pte) {
        .flags = g_flag | PTE_V,
        .ppn = pagenum(pt)
    };
}


static inline struct pte null_pte(void) {
    return (struct pte) { };
}