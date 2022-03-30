#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);
struct opened_file {
  struct file *file;
  int fd;
  struct list_elem elem;
};
struct lock file_lock;

#endif /**< userprog/syscall.h */
