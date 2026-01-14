# ECE 391 – Educational Operating System (RISC-V)

A Unix-like educational operating system built in C for the RISC-V architecture,
implementing virtual memory, process management, threading, filesystems,
device drivers, and system calls.

## Project Context

This repository is a **personal mirror** of a team project developed at the University of Illinois
Urbana-Champaign.

The original development occurred in a course organization repository.
This copy exists for **portfolio and demonstration purposes**.

## System Overview

The operating system supports:
- Preemptive multitasking with kernel threads
- User processes with virtual memory isolation (Sv39)
- A Unix-style file abstraction layer
- Block and character device drivers
- A system call interface for user programs
- Basic userland applications (e.g., shells, games, utilities)

The OS runs under QEMU on a RISC-V platform and is written entirely in C,
with small amounts of assembly for context switching and trap handling.

## Technical Highlights

- Implemented user-mode memory isolation using RISC-V Sv39 paging
- Debugged and resolved store page faults caused by incorrect address space switching
- Designed interrupt-driven block I/O using VirtIO queues
- Built a thread-safe filesystem supporting concurrent access
- Ran and validated classic Unix programs and games on the OS

## My Contributions

I was responsible for designing and implementing major portions of the kernel,
including:

- **Threading and Scheduling**
  - Kernel thread creation, lifecycle management, and context switching
  - Blocking synchronization using condition variables and locks
  - Debugging race conditions and scheduler edge cases

- **Virtual Memory**
  - Sv39 page table setup and management
  - Lazy allocation and user-mode page fault handling
  - Virtual-to-physical address mapping utilities

- **Filesystem (KTFS)**
  - Inode-based filesystem implementation
  - File read/write operations with offset support
  - Directory traversal and metadata handling
  - Thread-safe access using global and fine-grained locks

- **Device Drivers**
  - VirtIO block device driver
  - Interrupt-driven I/O with condition-based wakeups
  - Multi-block read/write support and correctness debugging

- **Process Management & Syscalls**
  - ELF loading and process initialization
  - User stack setup and trap frame handling
  - System calls for file I/O, process control, and device access

## What I Learned

This project strengthened my understanding of:
- How operating systems manage memory and isolation
- The complexity of concurrency and synchronization
- Hardware–software interaction via interrupts and device drivers
- Debugging low-level systems where traditional tools are limited
