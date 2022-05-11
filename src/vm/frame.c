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

void frame_table_init (void)
{
    evict_pt = NULL;
    lock_init (&ft_lock);
    list_init (&frame_table);
}

/* for debugging purpose only */
static struct frame* vm_get_fe (void *frame)
{
    //lock_acquire (&ft_lock);
    for (struct list_elem *e = list_begin (&frame_table); e != list_end (&frame_table); e = list_next (e))
    {
        struct frame *f = list_entry (e, struct frame, elem);
        if (f->frame == frame)
        {
            lock_release (&ft_lock);
            return f;
        }
    }
    //lock_release (&ft_lock);
    return NULL;
}

/**
 * Get a frame from physical memory.
 * Automatically evict one if no space left.
 */
void* get_frame (struct spl_pe *pe)
{
    void *ret = palloc_get_page (PAL_USER);
    if (ret != NULL)
    {
        add_frame (ret, pe);      
        return ret;
    }
    //return ret;
    //PANIC ("Eviction not implemented yet.");
    
    struct frame *fe;
    uint32_t *page_table;
    lock_acquire (&ft_lock);
    ASSERT (!list_empty (&frame_table));
    // BEWARE: deadlock here 
    while (true)
    {
        evict_pt = next_frame (evict_pt);
        fe = list_entry (evict_pt, struct frame, elem);
        page_table = fe->thread->pagedir;
        if (!pagedir_is_accessed (page_table, fe->spl_pe->upage))
        {
            ASSERT (ret == NULL);
            if (evict (fe, fe->spl_pe))
                ret = fe->frame;
            break;
        }
        pagedir_set_accessed (page_table, fe->spl_pe->upage, false);
    }
    lock_release (&ft_lock);
    return ret;
}

/** 
 * Add frame entry into frame table. 
*/
void add_frame (void *frame, struct spl_pe *pe)
{
    struct frame *f = malloc (sizeof (struct frame));
    f->frame = frame;
    f->spl_pe = pe;
    f->tid = thread_current ()->tid;
    f->thread = thread_current ();
    
    lock_acquire (&ft_lock);
    ASSERT (vm_get_fe (frame) == NULL);
    list_push_back (&frame_table, &f->elem);
    lock_release (&ft_lock);
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
    PANIC ("vm_remove_fe: user frame not found in frame table");
}

/**
 * Swap out the given frame 
 * Need to assume that ft_lock is held
 */
static bool evict (struct frame *f, struct spl_pe *pe)
{
    uint32_t *page_table = thread_current ()->pagedir;
    
    /* If not dirty or read-only, no need to evict */
    if (!pagedir_is_dirty (page_table, pe->upage))
    {
        pe->present = false;
        return true;
    }

    /* Save the page in swap device */
    size_t slot = swap_out (f->frame);
    if (slot == BITMAP_ERROR)
        return false;
        
    /* Change the corresponding spl_pe */
    struct lock *spl_pt_lock = &f->thread->process->spl_pt_lock;
    lock_acquire (spl_pt_lock);
    pe->type = PG_SWAP;
    pe->present = false;
    pe->slot = slot;
    lock_release (spl_pt_lock);

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