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
    // FIX ME
    struct elf64_ehdr ehdr; // variable to hold the elfheader

    // moving to the start if the ELF file with offset 0
    ioseek(elfio, 0);

    // reading elfheader from the elf file into memory
    // if the read fails, then return an IO error
    if (ioread(elfio, &ehdr, sizeof(ehdr)) != sizeof(ehdr)) {
        return -EIO;
    }
    
    // checking elf magic numbers to ensure that this is an ELF file
    // if it isnn't, return a bad format error
    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' ||ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F') {
        return -EBADFMT;
    }

    // checking that that the file is 64-bit ELF
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        return -EINVAL;
    }

    // checking that the file is in little-endian format
    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        return -EINVAL;
    }

    // checking that the file is using the current ELF version
    if (ehdr.e_ident[EI_VERSION] != EV_CURRENT) {
        return -EINVAL;
    }

    // checking that the file fits riscv architecture
    if (ehdr.e_machine != EM_RISCV) {
        return -EINVAL;
    }

    // checking that the file is executable
    if (ehdr.e_type != ET_EXEC) {
        return -EINVAL;
    }
}