#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "filesys/file.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "userprog/syscall.h"
typedef int pid_t;

struct process {
    int exit_status;
    bool load_success;
    bool running;
    bool free_self;
    pid_t pid;
    struct thread *thread;
    struct semaphore wait_for_process;
    struct semaphore loading;
    struct list_elem all_elem;
    struct list_elem elem;

    struct list childs;
    struct file *executable;

    struct list opened_files;
    int min_available_fd;
    struct opened_file *fd_table[130];
};

void process_init (void);
struct process* process_create (void);
tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);
struct process *get_process_by_pid (pid_t);
struct process *process_current (void);

#endif /**< userprog/process.h */
