#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"
typedef int pid_t;

struct process {
    int exit_status;
    struct thread *father_thread;
    struct thread *thread;
    struct semaphore wait_for_process;
    struct list_elem all_elem;
    struct list_elem elem;
    struct list childs;
};

void process_init (void);
struct process* process_create (void);
tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /**< userprog/process.h */
