/*! @file syscall.c
    @brief system call handlers 
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA
*/



#ifdef SYSCALL_TRACE
#define TRACE
#endif

#ifdef SYSCALL_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "scnum.h"
#include "process.h"
#include "memory.h"
#include "io.h"
#include "device.h"
#include "fs.h"
#include "intr.h"
#include "timer.h"
#include "error.h"
#include "thread.h"
#include "process.h"
#include "ioimpl.h"
#include "ktfs.h"
#include "riscv.h"

#define MAX_PRINT_LEN 512  
#define NEXT_RISCV_INSTRUCTION 4 //each instruction is 4 bytes wide

// EXPORTED FUNCTION DECLARATIONS
//

extern int ktfs_create(const char* name);
extern int ktfs_delete(const char* name);
extern void handle_syscall(struct trap_frame * tfr); // called from excp.c

// INTERNAL FUNCTION DECLARATIONS
//

static int64_t syscall(const struct trap_frame * tfr);
static int sysexit(void);
static int sysexec(int fd, int argc, char ** argv);
static int sysfork(const struct trap_frame * tfr);
static int syswait(int tid);
static int sysprint(const char * msg);
static int sysusleep(unsigned long us);
static int sysdevopen(int fd, const char * name, int instno);
static int sysfsopen(int fd, const char * name);
static int sysclose(int fd);
static long sysread(int fd, void * buf, size_t bufsz);
static long syswrite(int fd, const void * buf, size_t len);
static int sysioctl(int fd, int cmd, void * arg);
static int syspipe(int * wfdptr, int * rfdptr);
static int sysfscreate(const char* name); 
static int sysfsdelete(const char* name);

// EXPORTED FUNCTION DEFINITIONS
//

// void handle_syscall(struct trap_frame * tfr)
// Inputs: struct trap_frame *tfr - Trap frame containing syscall arguments
// Outputs: None
// Description: Dispatches a syscall based on the syscall number in tfr->a7 and sets the return value in tfr->a0.
// Side Effects: Advances the program counter to skip the syscall instruction
void handle_syscall(struct trap_frame * tfr) {    
    int64_t result = syscall(tfr); // Initiates syscall present in trap frame struct
    tfr->a0 = result; // Setting result into return address
    tfr->sepc += NEXT_RISCV_INSTRUCTION; // advancing PC to skip ecall and go to ret
}

// INTERNAL FUNCTION DEFINITIONS
//

// int allocate_fd(int fd, struct io * io)
// Inputs: int fd - Desired file descriptor or -1 for automatic allocation
//         struct io * io - I/O object to bind to the descriptor
// Outputs: int fd - Assigned file descriptor or error code
// Description: Allocates an entry in the current process's I/O table
// Side Effects: Modifies the process's I/O table
static int allocate_fd(int fd, struct io * io) {
    struct process * proc = current_process();

    if (fd == -1) {
        for (int i = 0; i < PROCESS_IOMAX; i++) {
            if (proc->iotab[i] == NULL) {
                proc->iotab[i] = io;
                return i;
            }
        }
        return -EMFILE; // no free slot
    }

    if (fd < 0 || fd >= PROCESS_IOMAX)
        return -EBADFD;

    if (proc->iotab[fd] != NULL)
        return -EBADFD;

    proc->iotab[fd] = io;
    return fd;
}



// int64_t syscall(const struct trap_frame * tfr)
// Inputs: const struct trap_frame *tfr - Trap frame containing syscall number and arguments
// Outputs: int64_t - Result of the syscall
// Description: Handles dispatch for all defined system calls
// Side Effects: Depends on the specific syscall invoked
int64_t syscall(const struct trap_frame * tfr) {
    kprintf("SYSCALL #%ld, a0=%p, a1=%p, a2=%p\n", tfr->a7, tfr->a0, tfr->a1, tfr->a2);
    switch(tfr->a7) { // a7 is where the system call number is
        case(SYSCALL_EXIT):
            return sysexit();
        case(SYSCALL_EXEC):
            return sysexec((int)tfr->a0, (int)tfr->a1, (char **)tfr->a2);
        case(SYSCALL_FORK):
            return sysfork(tfr);
        case(SYSCALL_WAIT):
            return syswait((int)tfr->a0);
        case(SYSCALL_PRINT):
            return sysprint((const char *)tfr->a0);
        case(SYSCALL_USLEEP):
            return sysusleep((unsigned long)tfr->a0);
        case(SYSCALL_DEVOPEN):
            return sysdevopen((int)tfr->a0, (const char *)tfr->a1, (int)tfr->a2);
        case(SYSCALL_FSOPEN):
            return sysfsopen((int)tfr->a0, (const char *)tfr->a1);
        case(SYSCALL_CLOSE):
            return sysclose((int)tfr->a0);
        case(SYSCALL_READ):
            return sysread((int)tfr->a0, (void *) tfr->a1, (size_t)tfr->a2);
        case(SYSCALL_WRITE):
            return syswrite((int)tfr->a0, (const void *) tfr->a1, (size_t)tfr->a2);
        case(SYSCALL_IOCTL):
            return sysioctl((int)tfr->a0, (int) tfr->a1, (void *)tfr->a2);
        case(SYSCALL_PIPE):
            return syspipe((int *)tfr->a0, (int *) tfr->a1);
        case(SYSCALL_FSCREATE):
            return sysfscreate((const char*) tfr->a0);
        case(SYSCALL_FSDELETE):
            return sysfsdelete((const char*) tfr->a0);
        default:
            return -ENOTSUP;

    }
}

// int sysexit(void)
// Inputs: None
// Outputs: int - Always returns 0
// Description: Terminates the current process
// Side Effects: Cleans up process state and exits current thread
int sysexit(void) {
    fsflush();
    process_exit(); // Exits the currently running process
    return 0; 
}

// int sysexec(int fd, int argc, char ** argv)
// Inputs: int fd - File descriptor of executable
//         int argc - Argument count
//         char **argv - Argument vector
// Outputs: int - Result of process_exec or error
// Description: Replaces current process image with new executable
// Side Effects: Resets memory space and jumps to new user code
int sysexec(int fd, int argc, char ** argv) {
    struct io * exeio = process_get_io(fd); // recovering io pointer
    if (!exeio) return -EBADFD; // if nothing returns, error
    return process_exec(exeio, argc, argv); // begin execution
}

// int sysfork(const struct trap_frame * tfr)
// Inputs: const struct trap_frame *tfr - Trap frame of parent process
// Outputs: int - Child thread ID in parent, 0 in child, or error
// Description: Creates a new child process with duplicated address space and state
// Side Effects: Allocates memory, updates process table, spawns thread
int sysfork(const struct trap_frame * tfr) {
    return process_fork(tfr); 
}

// int syswait(int tid)
// Inputs: int tid - Thread ID to wait for
// Outputs: int - 0 on success or error code
// Description: Waits for the specified thread to exit
// Side Effects: May sleep until the thread exits
int syswait(int tid) {
    return thread_join(tid); // Sleeps until a specified child process completes
}

// int sysprint(const char * msg)
// Inputs: const char *msg - Null-terminated message string
// Outputs: int - 0 on success or error code
// Description: Prints a message from user space prefixed with thread ID
// Side Effects: Writes to console output

int sysprint(const char * msg) {
    // validate_vstr returns 0 on success, or negative error (e.g., -EFAULT)
    int rc = validate_vstr(msg, PTE_U | PTE_R);
    if (rc)
        return -rc;

    // Format: <thread_name:thread_num> msg
    kprintf("<%s:%d> %s\n", running_thread_name(), running_thread(), msg);
    return 0;
}

// int sysusleep(unsigned long us)
// Inputs: unsigned long us - Number of microseconds to sleep
// Outputs: int - Always returns 0
// Description: Suspends the calling thread for specified time
// Side Effects: Puts the thread to sleep
int sysusleep(unsigned long us) {
    struct alarm al;
    alarm_init(&al, "sysusleep");
    alarm_sleep_us(&al, us);
    return 0;
}

// int sysdevopen(int fd, const char * name, int instno)
// Inputs: int fd - File descriptor or -1 for automatic allocation
//         const char *name - Name of the device
//         int instno - Instance number of device
// Outputs: int - File descriptor or error code
// Description: Opens a device and associates it with a file descriptor
// Side Effects: May allocate a new device I/O object
int sysdevopen(int fd, const char * name, int instno) {
    int rc = validate_vstr(name, PTE_U | PTE_R);
    if (rc)
        return -rc;

    struct io *io = NULL;
    rc = open_device(name, instno, &io); // opening device
    if (rc < 0)
        return rc;

    return allocate_fd(fd, io); // allocating new fd
}

// int sysfsopen(int fd, const char * name)
// Inputs: int fd - Desired file descriptor or -1 for auto-assign
//         const char *name - Name of the file in the filesystem
// Outputs: int - File descriptor or error code
// Description: Opens a file from KTFS and assigns to I/O table
// Side Effects: May allocate and reference a filesystem I/O object
int sysfsopen(int fd, const char * name) {
    int rc = validate_vstr(name, PTE_U | PTE_R);
    if (rc)
        return -rc;
    
    struct io *io;
    rc = fsopen(name, &io);
    if (rc < 0)
        return rc;

    return allocate_fd(fd, io); // allocating new fd
}

// int sysclose(int fd)
// Inputs: int fd - File descriptor to close
// Outputs: int - 0 on success or error
// Description: Closes the given file descriptor
// Side Effects: Removes entry from process I/O table and frees I/O
int sysclose(int fd) {
    struct io * io = process_get_io(fd); // recovering io pointer
    if (io == NULL)
        return -EBADFD;

    // clear the slot 
    current_process()->iotab[fd] = NULL;
    
    ioclose(io); // calling close from io abstraction
    return 0; //success
}

// long sysread(int fd, void * buf, size_t bufsz)
// Inputs: int fd - File descriptor
//         void *buf - Pointer to buffer
//         size_t bufsz - Number of bytes to read
// Outputs: long - Number of bytes read or error code
// Description: Reads from the file descriptor into user buffer
// Side Effects: May block on I/O or modify buffer content
long sysread(int fd, void * buf, size_t bufsz) {
    struct io * io = process_get_io(fd); // recovering io pointer
    if (io == NULL) return -EBADFD;

    int rc = validate_vptr(buf, bufsz, PTE_U | PTE_R);
    if (rc)
        return -rc;

    return io->intf->read(io, buf, bufsz); //calling from io abstraction
}

// long syswrite(int fd, const void * buf, size_t len)
// Inputs: int fd - File descriptor
//         const void *buf - Pointer to data
//         size_t len - Length of data
// Outputs: long - Number of bytes written or error code
// Description: Writes to the file descriptor from user buffer
// Side Effects: May block or alter I/O state
long syswrite(int fd, const void * buf, size_t len) {
    struct io * io = process_get_io(fd); // recovering io pointer
    if (io == NULL) return -EBADFD;

    int rc = validate_vptr(buf, len, PTE_U | PTE_W);
    if (rc)
        return -rc;

    // Handle small writes (< block size) via writeat so they don't get rejected
    int blksz = ioblksz(io);
    if (len > 0 && len < (size_t)blksz) {
        unsigned long long pos;
        if (ioctl(io, IOCTL_GETPOS, &pos) < 0)
            return -EIO;

        long w = iowriteat(io, pos, buf, len);
        if (w > 0) {
            pos += w;
            if (ioctl(io, IOCTL_SETPOS, &pos) < 0)
                return -EIO;
        }
        return w;
    }

    return io->intf->write(io, buf, len); //calling from io abstraction
}

// int sysioctl(int fd, int cmd, void * arg)
// Inputs: int fd - File descriptor
//         int cmd - IOCTL command
//         void *arg - Argument for command
// Outputs: int - Command-specific return or error
// Description: Sends a control request to an I/O device
// Side Effects: Device-specific behavior
int sysioctl(int fd, int cmd, void * arg) {
    struct io * io = process_get_io(fd); // recovering io pointer
    if (io == NULL) return -EBADFD;

    return io->intf->cntl(io, cmd, arg); //calling from io abstraction
}

// int sysfscreate(const char* name)
// Inputs: const char *name - Name of file to create
// Outputs: int - 0 on success or error
// Description: Creates a file in KTFS
// Side Effects: Alters filesystem state
int sysfscreate(const char* name) {
    int rc = validate_vstr(name, PTE_U | PTE_R); //validating string
    if (rc)
        return -rc;

    return fscreate(name);  // calling create from ktfs
}

// int sysfsdelete(const char* name)
// Inputs: const char *name - Name of file to delete
// Outputs: int - 0 on success or error
// Description: Deletes a file in KTFS
// Side Effects: Alters filesystem state
int sysfsdelete(const char* name) {
    int rc = validate_vstr(name, PTE_U | PTE_R); //validating string
    if (rc)
        return -rc;

    return fsdelete(name);  // calling delete from ktfs
}

// int sysiodup(int oldfd, int newfd)
// Inputs: int oldfd - Source file descriptor
//         int newfd - Target file descriptor
// Outputs: int - New file descriptor or error
// Description: Duplicates an open file descriptor
// Side Effects: Increments I/O refcount, closes newfd if open
int sysiodup(int oldfd, int newfd) {
    struct process *proc = current_process(); //getting current process

    // checking if fd is within bounds
    if (oldfd < 0 || oldfd >= PROCESS_IOMAX || newfd < 0 || newfd >= PROCESS_IOMAX)
        return -EINVAL;

    // if the oldfd does not have associated i/o, return error
    if (proc->iotab[oldfd] == NULL)
        return -EBADFD;

    // close target fd if already in use
    if (proc->iotab[newfd] != NULL) {
        ioclose(proc->iotab[newfd]);
    }

    // duplicate the descriptor
    proc->iotab[newfd] = proc->iotab[oldfd];
    ioaddref(proc->iotab[newfd]); // increment reference count
    return newfd;
}

// int syspipe(int *wfdptr, int *rfdptr)
// Inputs: int *wfdptr - Pointer to store write end fd
//         int *rfdptr - Pointer to store read end fd
// Outputs: int - 0 on success or error code
// Description: Creates a pipe and allocates two fds
// Side Effects: Modifies I/O table and allocates pipe I/O objects

int syspipe(int *wfdptr, int *rfdptr) {
    struct io *wio, *rio;
    int wfd, rfd;

    if (wfdptr == NULL || rfdptr == NULL)
        return -EINVAL;

    create_pipe(&wio, &rio);

    struct process *proc = current_process();
    for (wfd = 0; wfd < PROCESS_IOMAX && proc->iotab[wfd]; wfd++);
    for (rfd = 0; rfd < PROCESS_IOMAX && (proc->iotab[rfd] || rfd == wfd); rfd++);

    if (wfd >= PROCESS_IOMAX || rfd >= PROCESS_IOMAX) {
        ioclose(wio);
        ioclose(rio);
        return -EMFILE;
    }

    proc->iotab[wfd] = wio;
    proc->iotab[rfd] = rio;

    *wfdptr = wfd;
    *rfdptr = rfd;

    return 0;
}