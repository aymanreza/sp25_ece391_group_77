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

void handle_syscall(struct trap_frame * tfr) {    
    int64_t result = syscall(tfr); // Initiates syscall present in trap frame struct
    tfr->a0 = result; // Setting result into return address
    tfr->sepc += NEXT_RISCV_INSTRUCTION; // advancing PC to skip ecall and go to ret
}

// INTERNAL FUNCTION DEFINITIONS
//

// helper function for if fd=-1 and must be allocated to next available
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

int64_t syscall(const struct trap_frame * tfr) {
    kprintf("SYSCALL #%ld, a0=%p, a1=%p, a2=%p\n", tfr->a7, tfr->a0, tfr->a1, tfr->a2);
    switch(tfr->a7) { // a7 is where the system call number is
        case(SYSCALL_EXIT):
            return sysexit();
        case(SYSCALL_EXEC):
            return sysexec((int)tfr->a0, (int)tfr->a1, (char **)tfr->a2);
        case(SYSCALL_FORK):
            return sysfork((const struct trap_frame *)tfr->a0);
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

int sysexit(void) {
    fsflush();
    process_exit(); // Exits the currently running process
    return 0; 
}

int sysexec(int fd, int argc, char ** argv) {
    struct io * exeio = process_get_io(fd); // recovering io pointer
    if (!exeio) return -EBADFD; // if nothing returns, error
    return process_exec(exeio, argc, argv); // begin execution
}

int sysfork(const struct trap_frame * tfr) {
    return process_fork(tfr); 
}

int syswait(int tid) {
    return thread_join(tid); // Sleeps until a specified child process completes
}

int sysprint(const char * msg) {
    // validate_vstr returns 0 on success, or negative error (e.g., -EFAULT)
    int rc = validate_vstr(msg, PTE_U | PTE_R);
    if (rc)
        return -rc;

    // Format: <thread_name:thread_num> msg
    kprintf("<%s:%d> %s\n", running_thread_name(), running_thread(), msg);
    return 0;
}

int sysusleep(unsigned long us) {
    struct alarm al;
    alarm_init(&al, "sysusleep");
    alarm_sleep_us(&al, us);
    return 0;
}

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

int sysclose(int fd) {
    struct io * io = process_get_io(fd); // recovering io pointer
    if (io == NULL)
        return -EBADFD;

    // clear the slot 
    current_process()->iotab[fd] = NULL;
    
    ioclose(io); // calling close from io abstraction
    return 0; //success
}

long sysread(int fd, void * buf, size_t bufsz) {
    struct io * io = process_get_io(fd); // recovering io pointer
    if (io == NULL) return -EBADFD;

    int rc = validate_vptr(buf, bufsz, PTE_U | PTE_R);
    if (rc)
        return -rc;

    return io->intf->read(io, buf, bufsz); //calling from io abstraction
}

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

int sysioctl(int fd, int cmd, void * arg) {
    struct io * io = process_get_io(fd); // recovering io pointer
    if (io == NULL) return -EBADFD;

    return io->intf->cntl(io, cmd, arg); //calling from io abstraction
}

int syspipe(int * wfdptr, int * rfdptr) {
    return 0;
}

int sysfscreate(const char* name) {
    int rc = validate_vstr(name, PTE_U | PTE_R); //validating string
    if (rc)
        return -rc;

    return fscreate(name);  // calling create from ktfs
}

int sysfsdelete(const char* name) {
    int rc = validate_vstr(name, PTE_U | PTE_R); //validating string
    if (rc)
        return -rc;

    return fsdelete(name);  // calling delete from ktfs
}

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