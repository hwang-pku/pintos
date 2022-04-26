#include "vm/frame.h"
#include <list.h>
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/synch.h"

static struct list frame_table;
static struct lock ft_lock;

void vm_init (void)
{
    lock_init (&ft_lock);
    list_init (&frame_table);
}

/** 
 * Add frame entry into frame table. 
*/
void vm_add_fe (void *frame)
{
    struct frame *f = malloc (sizeof (struct frame));
    f->frame = frame;
    f->tid = thread_current ()->tid;
    
    lock_acquire (&ft_lock);
    list_push_back (&frame_table, &f->elem);
    lock_release (&ft_lock);
}

/**
 * Remove a frame entry from frame table
 */
void vm_remove_fe (void *frame)
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
            free (f);
            lock_release (&ft_lock);
            return ;
        }
    }
    PANIC ("vm_remove_fe: user frame not found in frame table");
}