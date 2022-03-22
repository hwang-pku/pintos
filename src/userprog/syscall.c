#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static void syscall_handler (struct intr_frame *);
static void check_ptr_validity (const void *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  printf ("system call!\n");
  thread_exit ();
}

static void 
check_ptr_validity (const void *ptr)
{
  if (ptr != NULL && is_user_vaddr (ptr) && ptr < PHYS_BASE)
    return;
  exit(-1);
  NOT_REACHED();
}