#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "threads/synch.h"
#include "filesys/directory.h"

typedef int pid_t;

struct opened_file {
  struct file *file;    /**< File handle. */
  int fd;               /**< File descriptor. */
  struct list_elem elem;/**< Elem for opened_file_list. */
  struct dir *dir;      /**< Directory of this file. */
};

void syscall_init (void);
struct lock file_lock;
#ifdef VM
bool try_load_multiple (const void*, unsigned);
#endif

#endif /**< userprog/syscall.h */
