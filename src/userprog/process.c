#include "userprog/process.h"

#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"

#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"

#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#include "vm/page.h"
#include "vm/frame.h"
#include "vm/mmap.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static void free_process (struct process*);
static struct list all_list;

/** Sets up process system. */
void
process_init (void)
{
  list_init (&all_list);
  lock_init (&file_lock);
}

/** Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy, *save_ptr, *real_file_name;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  real_file_name = palloc_get_page (0);
  if (real_file_name == NULL)
    return TID_ERROR;
  strlcpy (real_file_name, file_name, PGSIZE);
  real_file_name = strtok_r (real_file_name, " ", &save_ptr);

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (real_file_name, PRI_DEFAULT, 
                       start_process, fn_copy);
  if (tid == TID_ERROR)
    palloc_free_page (fn_copy); 
  palloc_free_page (real_file_name); 
  return tid;
}

/** A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  char *file_name = file_name_, *save_ptr;
  struct intr_frame if_;
  bool success;
  struct process *cur_process = thread_current ()->process;

  file_name = strtok_r(file_name, " ", &save_ptr);
  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  success = load (file_name, &if_.eip, &if_.esp);

  /* Argument Passing */
  if (success)
  {
    /* first push all the parameters argv onto the stack */
    int argc = 0;
    static char *argv[128];
    for (char *token = file_name; token != NULL;
        token = strtok_r (NULL, " ", &save_ptr))
    {
      size_t str_size = strlen(token)+1;
      if_.esp -= str_size;
      strlcpy((char *)if_.esp, token, str_size);
      argv[argc++] = if_.esp;
      if (argc >= 25)
      {
        success = false;
        goto failed;
      }
    }

    /* add NULL at the end of argv */
    argv[argc] = NULL;
    
    /* do word-align */
    if_.esp -= (unsigned int)if_.esp % 4;

    /* push argv[] onto stack */
    for (int i = argc; i >= 0; i--)
    {
      if_.esp -= 4;
      *((char **)if_.esp) = argv[i];
    }
    
    /* push argv, argc, and RET onto stack */
    if_.esp -= 12;
    *(void **)(if_.esp+8) = if_.esp+12;
    *(int *)(if_.esp+4) = argc;
    *(void **)(if_.esp) = NULL;
  }
    
  if (success)
  {
    lock_acquire (&file_lock);
    cur_process->executable = filesys_open(file_name);
    file_deny_write (cur_process->executable);
    lock_release (&file_lock);
  }

failed:
  cur_process->load_success = success;
  sema_up(&cur_process->loading);

  /* If load failed, quit. */
  palloc_free_page (file_name);
  if (!success) 
  {
    cur_process->exit_status = -1;
    thread_exit ();
  }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/** Create a struct process saving the process-related information. 
 * This function is only called within thread_create. */
struct process*
process_create (void)
{
  struct process *p = malloc (sizeof (struct process));
  if (p == NULL)
    return NULL;

  list_init (&p->childs);
  list_init (&p->opened_files);
  p->next_fd = 2;
  p->next_id = 0;

  sema_init (&p->wait_for_process, 0);
  sema_init (&p->loading, 0);
  list_push_back (&all_list, &p->all_elem);
  p->running = true;
  p->free_self = false;

  hash_init (&p->spl_page_table, hash_spl_pe, hash_less_spl_pe, NULL);
  hash_init (&p->mmap_table, hash_mmap_file, hash_less_mmap_file, NULL);

  if (!pintos_booted)
    p->cwd = NULL;
  else if (process_current () != NULL && process_current ()->cwd != NULL)
    p->cwd = dir_reopen(process_current ()->cwd);
  else
  {
    // PROBLEM HERE?
    intr_enable ();
    p->cwd = dir_open_root ();
    intr_disable ();
  }
  return p;  
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.
*/
int
process_wait (tid_t child_tid) 
{
  struct process *p = get_process_by_pid(child_tid);

  if (p == NULL)
    return -1;

  sema_down (&p->wait_for_process);

  int ret = p->exit_status;
  /* Free the child waited once it finished (only wait once). */
  if (thread_current ()->tid != 1)
    list_remove (&p->elem);
  free_process (p);

  return ret;
}

/** Free the current process's resources, 
 *  along with (possibly) its files and children.
 */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  struct process *pcur = cur->process;
  uint32_t *pd;

  printf("%s: exit(%d)\n", cur->name, pcur->exit_status);
  
  list_remove (&pcur->all_elem);
  
//#ifdef VM
  /* unmap all the mapped files */
  unmap_mmap_table (&pcur->mmap_table);
//#endif

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      /* must not do eviction when clearing page table */
      lock_acquire (&evict_lock);
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
      lock_release (&evict_lock);
    }

    /* Free the resources occupied by dead childs. */
  while (!list_empty(&pcur->childs))  
  {
    struct list_elem *e = list_front (&pcur->childs);
    ASSERT (e != NULL);
    struct process *child = list_entry (e, struct process, elem);
    list_remove (e);
    /* If child is still running, let it reap its own resources. */
    if (child->running)
      child->free_self = true;
    /* Else, reap the child. */
    else
      free_process (child);
  }

  /* file_close naturally allows writing to file. */
  if (pcur->load_success)
  {
    lock_acquire (&file_lock);
    file_close (pcur->executable);
    lock_release (&file_lock);
  }

  /* should be put here so frame could always refer to spl_pe */
  /* If set to free_self, reap this process. */
  if (pcur->free_self)
    free_process (pcur);
  else
  {
    /* If not, there could be a father process waiting. */
    pcur->running=false;
    sema_up (&pcur->wait_for_process);
  }
}

/** Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}


/** Get the child of current process by PID.
 *  If current process is the root process, then find from all_list. */
struct process*
get_process_by_pid (pid_t pid)
{
  struct process *cur_process = process_current ();
  if (cur_process != NULL)
  {
    for (struct list_elem *e = list_begin (&cur_process->childs); 
          e != list_end (&cur_process->childs); e = list_next (e))
    {
      struct process *p = list_entry (e, struct process, elem);
      if (p->pid == pid)
        return p;
    }
  }
  else
  {
    for (struct list_elem *e = list_begin (&all_list); 
          e != list_end (&all_list); e = list_next (e))
    {
      struct process *p = list_entry (e, struct process, all_elem);
      if (p->pid == pid)
        return p;
    }
  }
  return NULL;
}

/** A macro to get the current process. */
struct process*
process_current (void)
{
  return thread_current ()->process;
}

/** Free the resources occupied by current process.
 *  This includes its process frame and opened files. */
static void
free_process (struct process *p)
{
  lock_acquire (&file_lock);
  /* Close all the opened files. */
  while (!list_empty (&p->opened_files))
  {
    struct opened_file *f = list_entry (list_pop_front(&p->opened_files),
                                        struct opened_file, elem);
    file_close (f->file);
    free (f);
  }
  /* don't allow write to executable here since we do it in process_exit */
  lock_release (&file_lock);
  
  /* remove the frames occupied from frame table */
  hash_destroy (&p->spl_page_table, hash_free_spl_pe);
  hash_destroy (&p->mmap_table, hash_free_mmap_file);

  /* free the process. */
  free (p);
}

/** We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/** ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/** For use with ELF types in printf(). */
#define PE32Wx PRIx32   /**< Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /**< Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /**< Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /**< Print Elf32_Half in hexadecimal. */

/** Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/** Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/** Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /**< Ignore. */
#define PT_LOAD    1            /**< Loadable segment. */
#define PT_DYNAMIC 2            /**< Dynamic linking info. */
#define PT_INTERP  3            /**< Name of dynamic loader. */
#define PT_NOTE    4            /**< Auxiliary info. */
#define PT_SHLIB   5            /**< Reserved. */
#define PT_PHDR    6            /**< Program header table. */
#define PT_STACK   0x6474e551   /**< Stack segment. */

/** Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /**< Executable. */
#define PF_W 2          /**< Writable. */
#define PF_R 4          /**< Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/** Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  lock_acquire (&file_lock);
  file = filesys_open (file_name);
  lock_release (&file_lock);

  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  return success;
}

/** load() helpers. */

/** Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/** Loads a segment starting at offset OFFSET in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFFSET.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t offset, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (offset % PGSIZE == 0);
  struct hash *pt = &process_current ()->spl_page_table;

  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;
      enum page_type type = page_read_bytes == PGSIZE ? PG_FILE 
                          : (page_zero_bytes == PGSIZE ? PG_ZERO : PG_MISC);
      
      /* Add a page to supplementary page table.
         Read from file when a page fault happens. */
      add_spl_pe (type, pt, file, offset, 
                  upage, page_read_bytes, page_zero_bytes, writable);
      
      /* Advance. */
      offset += page_read_bytes;
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/** Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  struct hash *pt = &process_current ()->spl_page_table;
  
  /* Add stack page onto supplementary page table. */
  add_spl_pe (PG_ZERO, pt, NULL, 0, ((uint8_t *)PHYS_BASE) - PGSIZE, 
              0, PGSIZE, true);

  /* Load page to allow for argument passing. */
  *esp = PHYS_BASE;
  return load_page (((uint8_t *)PHYS_BASE) - PGSIZE, true);
}
