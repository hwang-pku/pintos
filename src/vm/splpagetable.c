#include <stdio.h>
#include <hash.h>
#include <string.h>
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/off_t.h"
#include "vm/splpagetable.h"
#include "vm/frame.h"
#include "vm/swap.h"

static struct spl_pe* find_spl_pe (struct hash*, uint8_t*);
static bool install_page (void *, void *, bool);

unsigned hash_spl_pe (const struct hash_elem *e, void *aux UNUSED)
{
    struct spl_pe *pe;
    pe = hash_entry (e, struct spl_pe, elem);    
    return hash_int ((int) pe->upage);
}

bool hash_less_spl_pe (const struct hash_elem *a,
                       const struct hash_elem *b, void *aux UNUSED)
{
    return hash_entry (a, struct spl_pe, elem)->upage 
         < hash_entry (b, struct spl_pe, elem)->upage;
}

void hash_free_spl_pe (struct hash_elem *e, void *aux UNUSED)
{
    free (hash_entry (e, struct spl_pe, elem));
}


/**
 * Load page at UPAGE on page table
 * Could be stack or file
 * Could be in swap, demand zero or simply needs to read from file
 * If no frame is available, try to do eviction in palloc_get_page
 */
bool load_page (uint8_t *upage)
{
    ASSERT (pg_ofs (upage) == 0);
    struct hash *spl_pt = &process_current ()->spl_page_table;
    struct spl_pe *pe = find_spl_pe (spl_pt, upage);
    /* If upage not in spt */
    if (pe == NULL)
    {
        //printf ("load_page: page not in supplementary page table\n");
        /* It could be that upage is a stack page not allocated
           If out of valid stack space */
        return false;
    }
    ASSERT (pe->upage == upage);

    /* If access issues */
    if (pe->present && pe->writable == false)
        return false;

    uint8_t *kpage = get_frame (pe);
    if (kpage == NULL)
    {
        //printf ("load_page: page allocation failed\n");
        //palloc_free_page (kpage);
        return false;
    }
    ASSERT (pg_ofs (kpage) == 0);
    ASSERT (!pe->present);
    pe->kpage = kpage;

    /* If page is in swap */
    if (pe->type == PG_SWAP)
    {
        //PANIC ("load_page: swap not implemented");
        swap_in (pe->slot, kpage);
    }
    else
    {
        /* Load this page. */
        if (pe->type == PG_MISC || pe->type == PG_FILE)
        {
            //printf ("%d+%d=%d\n", pe->read_bytes, pe->zero_bytes, PGSIZE);
            ASSERT (pe->file != NULL);
            ASSERT (pe->read_bytes + pe->zero_bytes == PGSIZE);
            lock_acquire (&file_lock);
            file_seek (pe->file, pe->offset);
            if (file_read (pe->file, kpage, pe->read_bytes) != (int) pe->read_bytes)
            {
                lock_release (&file_lock);
                //printf ("load_page: unable to read file for required bytes");
                palloc_free_page (kpage);
                return false; 
            }
            lock_release (&file_lock);
        }
        memset (kpage + pe->read_bytes, 0, pe->zero_bytes);
    
        /* Add the page to the process's address space. */
        if (!install_page (upage, kpage, pe->writable)) 
        {
            //printf ("load_page: page installation failed\n");
            palloc_free_page (kpage);
            return false; 
        }
    }
    pe->present = true;
    return true;
}

bool add_spl_pe (enum page_type type, struct hash *spl_pt, struct file *file, off_t offset, 
                 uint8_t *upage, uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
    struct spl_pe *pe = malloc (sizeof (struct spl_pe));
    if (pe == NULL)
        return false;
    pe->type = type;
    pe->file = file;
    pe->offset = offset;
    pe->upage = upage;
    pe->kpage = NULL;
    pe->read_bytes = read_bytes;
    pe->zero_bytes = zero_bytes;
    pe->writable = writable;
    pe->present = false;
    pe->slot = -1;

    struct lock *spl_pt_lock = &thread_current ()->process->spl_pt_lock;
    lock_acquire (spl_pt_lock);
    if (hash_insert (spl_pt, &pe->elem) != NULL)
    {
        /* If entry already in supplementary page table */
        lock_release (spl_pt_lock);
        free (pe);
        return false;
    }
    lock_release (spl_pt_lock);
    return true;
}

bool is_writable (const void *upage)
{
    struct hash *spl_pt = &process_current ()->spl_page_table;
    struct spl_pe *pe = find_spl_pe (spl_pt, pg_round_down (upage));
    ASSERT (pe != NULL);
    return pe -> writable;
}

static struct spl_pe* find_spl_pe (struct hash *hash, uint8_t *upage)
{
    struct spl_pe pe;
    pe.upage = upage;
    struct lock *spl_pt_lock = &thread_current ()->process->spl_pt_lock;
    lock_acquire (spl_pt_lock);
    struct hash_elem *p = hash_find(hash, &pe.elem);
    lock_release (spl_pt_lock);
    return p ? hash_entry(p, struct spl_pe, elem) : NULL;
}

static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

