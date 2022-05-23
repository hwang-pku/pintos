#include "userprog/process.h"
#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include <kernel/stdio.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "userprog/pagedir.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "vm/splpagetable.h"
#include "vm/frame.h"

#define MAX_FD 129
#define FD_ERROR -1

/* The number of parameters required for each syscall. */
int syscall_param_num[13] = {0, 1, 1, 1, 2, 1, 1, 1, 3, 3, 2, 1, 1};

static void syscall_handler (struct intr_frame *);

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

static struct opened_file* get_opened_file_by_fd (int);
static void check_ptr_validity (const void*);
static void check_mem_validity (const void*, size_t);
static void check_str_validity (const char*);
static bool try_load_page (const void*);
static bool try_load_multiple (const void*, unsigned);
static bool is_seg_writable (const void*, unsigned);
static void reset_evictability (const void*, unsigned);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}


static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  check_mem_validity (f->esp, 4);
  int syscall_num = *(int*)f->esp;

  /* check if the parameters are valid. */
  int param_num = syscall_param_num[syscall_num];
  check_mem_validity (f->esp, (param_num + 1) * 4);

  /* extract the location of the parameters. */
  void *args[3];
  for (int i=1; i<=param_num; i++)
    args[i-1] = f->esp + i * 4;
  
  /** call corresponding syscalls according to syscall_num 
   *  extracted from interrupt frame. */
  switch (syscall_num)
  {
    case SYS_HALT: halt (); 
    case SYS_EXIT: exit (*(int*)args[0]); 
    case SYS_EXEC: 
      f->eax = exec (*(const char**)args[0]);
      break;
    case SYS_WAIT: 
      f->eax = wait (*(pid_t*)args[0]); break;
    case SYS_CREATE: 
      f->eax = create (*(const char**)args[0], *(unsigned*)args[1]); break;
    case SYS_REMOVE: f->eax = remove (*(const char**)args[0]); break;
    case SYS_OPEN: f->eax = open (*(const char**)args[0]); break;
    case SYS_FILESIZE: f->eax = filesize (*(int*)args[0]); break;
    case SYS_READ: 
      f->eax = read (*(int*)args[0], *(void**)args[1], *(unsigned*)args[2]);
      break;
    case SYS_WRITE:
      f->eax = write (*(int*)args[0], *(void**)args[1], *(unsigned*)args[2]);
      break;
    case SYS_SEEK: seek (*(int*)args[0], *(unsigned*)args[1]); break;
    case SYS_TELL: f->eax = tell (*(int*)args[0]); break;
    case SYS_CLOSE: close (*(int*)args[0]); break;
    default: NOT_REACHED ();
  }
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

/** Runs the executable whose name is given in cmd_line, 
 *  passing any given arguments, 
 *  and returns the new process's program id (pid). 
 * 
 *  If the program cannot load or run for any reason, must return pid -1, 
 *  which otherwise should not be a valid pid. */
static pid_t 
exec (const char *cmd_line) 
{
  check_str_validity (cmd_line);
  pid_t pid = process_execute (cmd_line);
  if (pid == TID_ERROR) return -1;
  /** wait for the process to finish loading to decide 
   *  if the loading succeeded. */
  struct process *p = get_process_by_pid (pid);
  sema_down (&p->loading);
  if (!p->load_success) return -1;
  return pid;
}

/** Waits for a child process pid and retrieves the child's exit status. */
static int 
wait (pid_t pid)
{
  return process_wait (pid);
}

/** Creates a file with the given file name. */
static bool
create (const char *file, unsigned initial_size)
{
  /* Check if all memory indicated is valid. */
  check_mem_validity (file, sizeof (const char*));

  /* Create the file. */
  lock_acquire (&file_lock);
  bool ret = filesys_create (file, initial_size);
  lock_release (&file_lock);
  return ret;
}

/** Opens a file with the given file name.
 *  Returns the fd of the opened file, FD_ERROR is something goes wrong. */
static int 
open (const char *file) 
{
  check_str_validity (file);
  struct process *cur_process = process_current ();

  /* open the file in filesys. */
  lock_acquire (&file_lock);
  struct file *tmp_f = filesys_open (file);
  lock_release (&file_lock);

  if (tmp_f == NULL)
    return FD_ERROR;

  /* allocate an entry in the fd table for the file. */
  struct opened_file * f = malloc (sizeof (struct opened_file));
  if (f == NULL)
    return FD_ERROR;

  f->fd = cur_process->next_fd++;
  f->file = tmp_f;
  list_push_back (&cur_process->opened_files, &f->elem);

  return f->fd;
}

/** Reads size bytes from the file open as fd into buffer. 
 *  Returns the number of bytes actually read (0 at end of file), 
 *  or -1 if the file could not be read 
 *  (due to a condition other than end of file). 
 * 
 *  Fd 0 reads from the keyboard using input_getc(). */
static int
read (int fd, void *buffer, unsigned size)
{
  /* Install pages automatically */
  if (!try_load_multiple (buffer, size) || !is_seg_writable (buffer, size))
    exit (-1);

  /* if STDIN, just read from the console. */
  if (fd == 0)
  {
    int i = 0;
    for (uint8_t ch = input_getc (); ch != '\r' && i < (uint8_t) size; 
         i++, ch = input_getc ())
      *(uint8_t*)(buffer + i) = ch;
    reset_evictability (buffer, size);
    return i;
  }

  struct opened_file *of = get_opened_file_by_fd (fd);

  lock_acquire (&file_lock);
  int real_size = file_read (of->file, buffer, size);
  lock_release (&file_lock);
  reset_evictability (buffer, size);
  return real_size;
}

/** Writes size bytes from buffer to the open file fd. 
 *  Returns the number of bytes actually written.
 * 
 *  Fd 1 writes to the console. 
 */
static int
write (int fd, const void *buffer, unsigned size)
{
  /* Install pages automatically */
  if (!try_load_multiple (buffer, size))
    exit (-1);

  /* if STDOUT, just write to the console. */
  if (fd == 1)  
  {
    putbuf ((const char*)buffer, size);
    reset_evictability (buffer, size);
    return size;
  }

  struct opened_file *of = get_opened_file_by_fd (fd);
  if (of == NULL || of->file == NULL)
  {
    reset_evictability (buffer, size);
    return -1;
  }

  lock_acquire (&file_lock);
  int real_size = file_write (of->file, buffer, size);
  lock_release (&file_lock);
  reset_evictability (buffer, size);
  return real_size;
}

/** Check if the all the memory piece indicated is valid.
 *  Check for start and end and each pages in between. */
static void
check_mem_validity (const void *ptr, size_t size)
{
  for (size_t i=0; i*PGSIZE <= size; i++)
    check_ptr_validity (ptr + i*PGSIZE);
  check_ptr_validity (ptr + size);
}

/** Check if the pointer is valid. */
static void 
check_ptr_validity (const void *ptr)
{
  if (ptr != NULL && is_user_vaddr (ptr) 
      && pagedir_get_page (thread_current ()->pagedir, ptr) != NULL)
    return;
  exit(-1);
  NOT_REACHED ();
}

/** Check if the whole string is valid. 
 *  Check char by char until reaching '\0'. */
static void
check_str_validity (const char *ptr)
{
  check_mem_validity (ptr, sizeof(char));
  for (int i=0;; i++)
  {
    check_mem_validity (ptr + i, sizeof(char));
    if (*(ptr + i) == '\0')
      break;
  }
}

/** remove a file from the file system. */
static bool 
remove (const char *file) 
{
  check_str_validity (file);
  lock_acquire (&file_lock);
  bool ret = filesys_remove (file);
  lock_release (&file_lock);
  return ret;
}

/** Get the size of a opened file. */
static int 
filesize (int fd)
{
  struct opened_file* file= get_opened_file_by_fd (fd);
  if (file == NULL)
    return -1;

  lock_acquire (&file_lock);
  int fz = file_length(file->file);
  lock_release (&file_lock);
  return fz;
}

/** Move the position of the opened file's handle to a new place. */
static void 
seek (int fd, unsigned position) 
{
  struct opened_file *of = get_opened_file_by_fd (fd);

  lock_acquire (&file_lock);
  file_seek (of->file, position);
  lock_release (&file_lock);
}

/** Check where the file handle is at. */
static unsigned 
tell (int fd) 
{
  struct opened_file *of = get_opened_file_by_fd (fd);

  lock_acquire (&file_lock);
  unsigned ret = file_tell(of->file);
  lock_release (&file_lock);
  return ret;
}

/* Close an opened file. */
static void 
close (int fd) 
{
  struct opened_file *of = get_opened_file_by_fd (fd);

  /* remove the file from fd table. */
  list_remove (&of->elem);

  lock_acquire (&file_lock);
  file_close (of->file);
  lock_release (&file_lock);
  free (of);
}

/** Get opened files by its fd in current process. */
static struct opened_file* 
get_opened_file_by_fd (int fd)
{
  struct process *cur_process = process_current ();
  if (list_empty (&cur_process->opened_files))
    exit (-1);
  /* iterate through the opened file table to find the file. */
  for (struct list_elem *e = list_front (&cur_process->opened_files);
       e != list_end (&cur_process->opened_files); e = list_next (e))
      {
        struct opened_file *f = list_entry (e, struct opened_file, elem);
        if (f->fd == fd)
        {
          if (f->file == NULL)
            exit (-1);
          return f;
        }
      }
  exit (-1);
  NOT_REACHED ();
}

/* Attempt to load multiple pages */
static bool try_load_multiple (const void *upage, unsigned size)
{
  for (unsigned tmp = 0; tmp < size/PGSIZE; tmp++)
    if (!try_load_page (upage + tmp * PGSIZE))
      return false;
  return try_load_page (upage + size);
}

/* Try to load a user page, returns success.
   Returns true if already present */
static bool try_load_page (const void *upage)
{
  if (upage == NULL || !is_user_vaddr (upage))
    return false;
  if (pagedir_get_page (thread_current ()->pagedir, upage) != NULL)
    return true;
  /* page not present */
  return load_page (pg_round_down (upage), false);
}

/* Check writability of a segment from supplementary page table. */
static bool is_seg_writable (const void *upage, unsigned size)
{
  for (unsigned tmp = 0; tmp < size/PGSIZE; tmp++)
    if (!is_writable (upage + tmp * PGSIZE))
      return false;
  return is_writable (upage + size);
}

static void reset_evictability (const void *buffer, unsigned size)
{
  for (unsigned tmp = 0; tmp < size/PGSIZE; tmp++)
    set_evictable (pg_round_down (buffer + tmp * PGSIZE));   
  set_evictable (pg_round_down (buffer + size));   
}