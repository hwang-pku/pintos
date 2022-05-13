#include "vm/frame.h"
#include <list.h>
#include <bitmap.h>
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "threads/palloc.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "vm/splpagetable.h"
#include "vm/swap.h"

static struct list frame_table;
static struct lock ft_lock;
static struct list_elem *evict_pt;
static struct list_elem* next_frame (struct list_elem*);
static struct list_elem* prev_frame (struct list_elem*);
static bool evict (struct frame*, struct spl_pe*);
static struct frame* get_frame_to_evict (void);
static struct frame* add_frame (void*, struct spl_pe*);

void frame_table_init (void)
{
    evict_pt = NULL;
    lock_init (&ft_lock);
    list_init (&frame_table);
    lock_init (&evict_lock);
}

/* for debugging purpose only */
struct frame* vm_get_fe (void *frame)
{
    for (struct list_elem *e = list_begin (&frame_table); e != list_end (&frame_table); e = list_next (e))
    {
        struct frame *f = list_entry (e, struct frame, elem);
        if (f->frame == frame)
        {
            lock_release (&ft_lock);
            return f;
        }
    }
    return NULL;
}

/**
 * Get a frame from physical memory.
 * Automatically evict one if no space left.
 */
struct frame* get_frame (struct spl_pe *pe)
{
    void *ret = palloc_get_page (PAL_USER);
    if (ret != NULL)
        return add_frame (ret, pe);      
    
    struct frame *fe = NULL;
    lock_acquire (&ft_lock);
    {
        ASSERT (!list_empty (&frame_table));
        lock_acquire (&evict_lock);
        {
            fe = get_frame_to_evict ();
            /* If eviction successful */
            if (!evict (fe, pe))
                fe = NULL;
        }
        lock_release (&evict_lock);
    }
    lock_release (&ft_lock);
    return fe;
}

/** 
 * Add frame entry into frame table. 
 * Synchronization secure.
*/
static struct frame*
add_frame (void *frame, struct spl_pe *pe)
{
    struct frame *f = malloc (sizeof (struct frame));
    f->frame = frame;
    f->spl_pe = pe;
    f->tid = thread_current ()->tid;
    f->thread = thread_current ();
    lock_init (&f->frame_lock);
    
    lock_acquire (&ft_lock);
    ASSERT (vm_get_fe (frame) == NULL);
    list_push_back (&frame_table, &f->elem);
    lock_release (&ft_lock);
    return f;
}

/**
 * Remove a frame entry from frame table
 */
void remove_frame (void *frame)
{
    struct list_elem *e;
    
    lock_acquire (&ft_lock);
    for (e = list_begin (&frame_table); e != list_end (&frame_table);
        e = list_next (e))
    {
        struct frame *f = list_entry (e, struct frame, elem);
        if (f->frame == frame)
        {
            list_remove (e);
            /* move back eviction pointer if the frame it is
               pointing to is going to be removed */
            if (e == evict_pt)
                evict_pt = prev_frame (evict_pt);
            free (f);
            lock_release (&ft_lock);
            return ;
        }
    }
    lock_release (&ft_lock);
    PANIC ("vm_remove_fe: user frame not found in frame table");
}

static struct frame* get_frame_to_evict (void)
{
    uint32_t *page_table;
    while (true)
    {
        evict_pt = next_frame (evict_pt);
        struct frame *fe = list_entry (evict_pt, struct frame, elem);
        page_table = fe->thread->pagedir;
        /* Use CLOCK algorithm */
        if (!pagedir_is_accessed (page_table, fe->spl_pe->upage))
            return fe;
        pagedir_set_accessed (page_table, fe->spl_pe->upage, false);
    }
    NOT_REACHED ();
}

/**
 * Swap out the given frame
 * F is the frame to evict, PE is the page entry to occupy F
 * Need to assume that ft_lock is held
 */
static bool evict (struct frame *f, struct spl_pe *pe)
{
    uint32_t *page_table = f->thread->pagedir;
    struct spl_pe *prev_pe = f->spl_pe;

    printf ("thread %d: upage %x(%x == %x)\n", f->thread->tid, prev_pe->upage, pagedir_get_page (page_table, prev_pe->upage), f->frame);
    ASSERT (pagedir_get_page (page_table, prev_pe->upage) != NULL);
    ASSERT (pagedir_get_page (page_table, prev_pe->upage) == f->frame);
    ASSERT (pe != NULL && f != NULL);
    size_t slot = BITMAP_ERROR;
    /* If dirty, need swapping out */
    if ((pagedir_is_dirty (page_table, prev_pe->upage) || prev_pe->type == PG_SWAP)
    && ((slot = swap_out (f->frame)) == BITMAP_ERROR))
        return false;
        
    /* Change the corresponding spl_pe */
    if (slot != BITMAP_ERROR)
    {
        prev_pe->type = PG_SWAP;
        prev_pe->slot = slot;
    }
    pagedir_clear_page (page_table, prev_pe->upage);
    prev_pe->present = false;
    prev_pe->kpage = NULL;

    // Change frame entry
    f->thread = thread_current ();
    f->spl_pe = pe;
    f->tid = thread_current ()->tid;

    return true;
}

static struct list_elem* next_frame (struct list_elem* frame_pt)
{
    if (frame_pt == NULL)
        return list_begin (&frame_table);
    ASSERT (frame_pt != list_end (&frame_table));
    frame_pt = list_next (frame_pt);
    return frame_pt == list_end (&frame_table) ? list_begin (&frame_table)
         : frame_pt;
}

static struct list_elem* prev_frame (struct list_elem* frame_pt)
{
    ASSERT (frame_pt != NULL);
    if (frame_pt == list_head (&frame_table))
        frame_pt = list_end (&frame_table);
    return list_prev (frame_pt);
}