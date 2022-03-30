#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

struct opened_file {
  struct file *file;    /**< File handle. */
  int fd;               /**< fd. */
  struct list_elem elem;/**< Elem for opened_file_list. */
};

void syscall_init (void);
struct lock file_lock;

#endif /**< userprog/syscall.h */
