#ifndef __FRAME_H
#define __FRAME_H
#include <list.h>
#include "threads/synch.h"
#include "threads/thread.h"
#include "vm/page.h"

/* frame entry, or FE */
struct frame{
    void *frame;            /**< the frame this FE represents */
    tid_t tid;              /**< TID of the thread holding the frame */
    struct spl_pe *spl_pe;  /**< the SPE of this frame */
    struct thread *thread;  /**< the thread holding this frame */
    struct list_elem elem;  /**< list elem */
    struct lock frame_lock; /**< lock for frame loading */
    bool evictable;         /**< allow pinning down */
};
struct lock evict_lock;

void frame_table_init (void);
void remove_frame (void*);
struct frame* get_frame (struct spl_pe*, bool);
void set_evictable (void*);
void set_unevictable (void*);
#endif
