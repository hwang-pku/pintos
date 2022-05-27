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
#include "vm/page.h"
#include "vm/frame.h"
#include "vm/swap.h"

static bool install_page (void *, void *, bool);
static bool load_frame (struct spl_pe*, void*);

/* hash functions required by hash table implementation */
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
 * If no frame is available, try to do eviction
 */
bool load_page (uint8_t *upage, bool evictable)
{
    ASSERT (pg_ofs (upage) == 0);
    bool success = false;
    struct hash *spl_pt = &process_current ()->spl_page_table;
    struct spl_pe *pe = find_spl_pe (spl_pt, upage);
    /* If upage not in spt */
    if (pe == NULL)
        goto done;
    ASSERT (pe->upage == upage);

    /* If access issues */
    if (pe->present)
    {
        printf("already present\n");
        goto done;
    }

    /* pe is the supplementary page entry that triggered PF */
    struct frame* frame = get_frame (pe, evictable);
    if (frame == NULL)
        goto done;

    ASSERT (!pe->present);
    pe->kpage = frame->frame;

    /* Load the content of the frame */
    if (!load_frame (pe, frame->frame))
    {
        palloc_free_page (frame->frame);
        goto done;
    }

    /* Add the page to the process's address space. */
    if (!install_page (upage, frame->frame, pe->writable)) 
    {
        palloc_free_page (frame->frame);
        goto done;
    }
    pe->present = true;
    lock_release (&frame->frame_lock);
    success = true;
done:
    return success;
}

bool grow_stack (void* upage, const void* esp UNUSED)
{
    ASSERT (pg_ofs (upage) == 0);
    bool success = false;
    if (upage < PHYS_BASE - STACK_SIZE)
        goto done;
    if (upage < esp - 4096)
        goto done;
    struct thread *pt = thread_current ();
    for (void *p = upage; pagedir_get_page(pt->pagedir, p)==NULL; p += PGSIZE)
        if (!add_spl_pe (PG_ZERO, &pt->process->spl_page_table, NULL, 0, 
                         p, 0, PGSIZE, true))
            goto done;
    if (!load_page (upage, true))
        goto done;
    success = true;
done:
    return success;
}


bool add_spl_pe (enum page_type type, struct hash *spl_pt, struct file *file,
                 off_t offset, uint8_t *upage, uint32_t read_bytes, 
                 uint32_t zero_bytes, bool writable)
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

    if (hash_insert (spl_pt, &pe->elem) != NULL)
    {
        /* If entry already in supplementary page table */
        free (pe);
        return false;
    }
    return true;
}

bool rm_spl_pe (uint8_t *upage)
{
    if (pg_ofs (upage) != 0)
        return false;
    
    struct hash *spt = &process_current ()->spl_page_table;
    struct spl_pe *pe = find_spl_pe (spt, upage);
    if (! (pe != NULL && hash_delete (spt, &pe->elem) != NULL))
        return false;
    free (pe);
    return true;
}

bool page_unmap (uint32_t *pd, uint8_t *upage)
{
    struct hash *spt = &process_current ()->spl_page_table;
    struct spl_pe *pe = find_spl_pe (spt, upage);
    ASSERT (pe != NULL);
    if (pagedir_get_page (pd, upage) && pagedir_is_dirty (pd, upage))
    {
        /* Write back the memory content */
        lock_acquire (&file_lock);
        if ((uint32_t) file_write_at (pe->file, upage, pe->read_bytes, 
                                    pe->offset) != pe->read_bytes)
            return false;
        lock_release (&file_lock);
    }
    /* Unmap the memory area */
    palloc_free_page (pagedir_get_page (pd, upage));
    pagedir_clear_page (pd, upage);
    if (hash_delete (spt, &pe->elem) == NULL)
        return false;
    free (pe);
    return true;
}

/* returns if a user page UPAGE is writable */
bool is_writable (const void *upage)
{
    struct hash *spl_pt = &process_current ()->spl_page_table;
    struct spl_pe *pe = find_spl_pe (spl_pt, pg_round_down (upage));
    ASSERT (pe != NULL);
    return pe -> writable;
}

/** Finds a supplementary page entry in HASH with the given UPAGE
 *  Needs external synchronization.
 */
struct spl_pe* find_spl_pe (struct hash *hash, uint8_t *upage)
{
    struct spl_pe pe;
    pe.upage = upage;
    struct hash_elem *p = hash_find(hash, &pe.elem);
    return p ? hash_entry(p, struct spl_pe, elem) : NULL;
}

/* Helper Functions */

/** 
 * map a WRITABLE user page UPAGE to kernel frame KPAGE
 */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

/**
 * Load the content of a frame according to the
 * supplementary table entry it corresponds to.
 * Needs frame table lock, frame lock and ste lock.
 */
static bool load_frame (struct spl_pe *pe, void *kpage)
{
    /* If page is in swap */
    if (pe->type == PG_SWAP)
        swap_in (pe->slot, kpage);
    else
    {
        /* Load this page. */
        if (pe->type == PG_MISC || pe->type == PG_FILE || pe->type == PG_MMAP)
        {
            ASSERT (pe->file != NULL);
            ASSERT (pe->read_bytes + pe->zero_bytes == PGSIZE);
            lock_acquire (&file_lock);
            off_t prev_pos = file_tell (pe->file);
            file_seek (pe->file, pe->offset);
            /* file read failed */
            if (file_read (pe->file, kpage, pe->read_bytes) 
                != (int) pe->read_bytes)
            {
                lock_release (&file_lock);
                return false; 
            }
            file_seek (pe->file, prev_pos);
            lock_release (&file_lock);
        }
        memset (kpage + pe->read_bytes, 0, pe->zero_bytes);
    }
    return true;    
}
