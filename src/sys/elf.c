// elf.c - ELF file loader
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#include "elf.h"
#include "conf.h"
#include "io.h"
#include "string.h"
#include "memory.h"
#include "assert.h"
#include "error.h"

#include <stdint.h>

// Offsets into e_ident

#define EI_CLASS        4   
#define EI_DATA         5
#define EI_VERSION      6
#define EI_OSABI        7
#define EI_ABIVERSION   8   
#define EI_PAD          9  


// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE     0
#define EV_CURRENT  1

// ELF header e_type values

enum elf_et {
    ET_NONE = 0,
    ET_REL,
    ET_EXEC,
    ET_DYN,
    ET_CORE
};

struct elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff; 
    uint32_t e_flags; 
    uint16_t e_ehsize; 
    uint16_t e_phentsize; 
    uint16_t e_phnum; 
    uint16_t e_shentsize; 
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

enum elf_pt {
	PT_NULL = 0, 
	PT_LOAD,
	PT_DYNAMIC,
	PT_INTERP,
	PT_NOTE,
	PT_SHLIB,
	PT_PHDR,
	PT_TLS
};

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// ELF header e_machine values (short list)

#define  EM_RISCV   243

int elf_load(struct io * elfio, void (**eptr)(void)) {
    struct elf64_ehdr ehdr;

    // Read ELF header at offset 0
    if (ioreadat(elfio, 0, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
        return -EIO;
    }

    // Validate ELF magic
    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
        return -EBADFMT;
    }

    // Sanity checks
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr.e_ident[EI_DATA] != ELFDATA2LSB ||
        ehdr.e_ident[EI_VERSION] != EV_CURRENT ||
        ehdr.e_machine != EM_RISCV ||
        ehdr.e_type != ET_EXEC) {
        return -EINVAL;
    }

    // Load each PT_LOAD segment
    for (int i = 0; i < ehdr.e_phnum; i++) {
        struct elf64_phdr phdr;

        // Read program header directly from correct offset
        uint64_t phdr_offset = ehdr.e_phoff + (i * sizeof(phdr));
        if (ioreadat(elfio, phdr_offset, &phdr, sizeof(phdr)) != sizeof(phdr)) {
            return -EIO;
        }

        if (phdr.p_type != PT_LOAD) continue;

        // Check memory range is within allowed region
        if (phdr.p_vaddr < UMEM_START_VMA || phdr.p_vaddr + phdr.p_memsz > UMEM_END_VMA) {
            return -EINVAL;
        }

        // variable for holding permission flags
        int flags = PTE_U;

        // determining additional permission flags
        if (phdr.p_flags & PF_R) {
            flags |= PTE_R;
        }

        if (phdr.p_flags & PF_W) {
            flags |= PTE_W;
        }

        if (phdr.p_flags & PF_X) {
            flags |= PTE_X;
        }

        // Save original flags
        int orig_flags = flags;

        // Always add write for loading
        int temp_flags = orig_flags | PTE_W;

        // Allocate and map with temp flags
        alloc_and_map_range(phdr.p_vaddr, phdr.p_memsz, temp_flags);

        // // allocating and mapping the region for this segment
        // alloc_and_map_range(phdr.p_vaddr, phdr.p_memsz, flags);

        // kprintf("Mapping segment: vaddr=0x%lx filesz=%lu memsz=%lu flags=0x%x\n",
        //     phdr.p_vaddr, phdr.p_filesz, phdr.p_memsz, flags);

        // Read program segment into memory
        if (ioreadat(elfio, phdr.p_offset, (void*)(uintptr_t)phdr.p_vaddr, phdr.p_filesz)
            != (int)phdr.p_filesz) {
            return -EIO;
        }

        // Zero any additional memory past file size
        memset((char*)(uintptr_t)phdr.p_vaddr + phdr.p_filesz, 0, phdr.p_memsz - phdr.p_filesz);

        // Reset to original flags (e.g., remove write from .text)
        set_range_flags((void*)phdr.p_vaddr, phdr.p_memsz, orig_flags);
    }

    // Set the entry point
    *eptr = (void (*)(void))(uintptr_t)ehdr.e_entry;

    return 0;
}