#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "threads/synch.h"

typedef int pid_t;

struct opened_file {
  struct file *file;    /**< File handle. */
  int fd;               /**< File descriptor. */
  struct list_elem elem;/**< Elem for opened_file_list. */
};

void syscall_init (void);
struct lock file_lock;
bool try_load_multiple (const void*, unsigned);

#endif /**< userprog/syscall.h */
