#include "userprog/process.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <kernel/stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "devices/input.h"

int syscall_param_num[13] = {0, 1, 1, 1, 2, 1, 1, 1, 3, 3, 2, 1, 1};

static void syscall_handler (struct intr_frame *);
static void check_ptr_validity (const void *);
static void halt (void);
static void exit (int status);
static pid_t exec (const char *cmd_line);
static int wait (pid_t pid);
static bool create (const char *file, unsigned initial_size);
static bool remove (const char *file);
static int open (const char *file);
static int filesize (int fd);
static int read (int fd, void *buffer, unsigned size);
static int write (int fd, const void *buffer, unsigned size);
static void seek (int fd, unsigned position);
static unsigned tell (int fd);
static void close (int fd);
static void check_mem_validity (const void *ptr, size_t size);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  check_ptr_validity (f->esp);
  int syscall_num = *(int*)f->esp;
  if (syscall_num == SYS_WRITE)
    write(*(int*)(f->esp+4), *(void **)(f->esp+8), *(unsigned*)(f->esp+12));
  else if (syscall_num == SYS_EXIT)
    exit(*(int*)(f->esp+4));
}

/** Halt the system by calling shutdown_power_off. */
static void 
halt (void)
{
  shutdown_power_off ();
  NOT_REACHED ();
}

/** Exits with a status. */
static void
exit (int status)
{
  struct process *p = thread_current ()->process;
  p->exit_status = status;
  thread_exit ();
  NOT_REACHED ();
}

/** Reads from a file descriptor. */
static int
read (int fd, void *buffer, unsigned size)
{
  return 0;
}

/* Write to a file descriptor. */
static int
write (int fd, const void *buffer, unsigned size)
{
  check_mem_validity (buffer, size);
  if (fd == 1)  
  {
    putbuf ((const char*)buffer, size);
    return size;
  }
  return -1;
}

static void
check_mem_validity (const void *ptr, size_t size)
{
  check_ptr_validity (ptr);
  check_ptr_validity (ptr+size);
  for (int i=0; i*PGSIZE <= size; i++)
    check_ptr_validity (ptr + i*PGSIZE);
}

static void 
check_ptr_validity (const void *ptr)
{
  if (ptr != NULL && is_user_vaddr (ptr) && ptr < PHYS_BASE)
    return;
  exit(-1);
  NOT_REACHED ();
}