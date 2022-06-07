#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include <hash.h>
#include "filesys/file.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"

struct process {
    int exit_status;                    /**< Exit status. */
    bool load_success;                  /**< If load successful. */
    bool running;                       /**< If still running. */
    bool free_self;                     /**< If need to free itself. */
    pid_t pid;                          /**< Process Id. */

    /** Sema to block the process waiting for this one. */
    struct semaphore wait_for_process;  
    struct semaphore loading;           /**< Sema indicate loading. */
    struct list_elem all_elem;          /**< Elem for all_list */
    struct list_elem elem;              /**< Elem for child in father. */

    struct list childs;                 /**< Child process. */
    /** The corresponding executable file */
    struct file *executable;            

    struct list opened_files;           /**< Opened file list. */
    int next_fd;                        /**< Next fd assigned. */
#ifdef VM
    int next_id;                        /**< Next Mmap Id assigned */
    struct hash spl_page_table;         /**< SPT */
    struct hash mmap_table;             /**< The table for mmap entries */
#endif

    struct dir *cwd;                    /**< CWD of this process. */
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
