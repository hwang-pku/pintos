#ifndef __FRAME_H
#define __FRAME_H
#include <list.h>
#include "threads/synch.h"
#include "threads/thread.h"
#include "vm/splpagetable.h"

struct frame{
    void *frame;
    tid_t tid;
    struct spl_pe *spl_pe;
    struct thread *thread;
    struct list_elem elem;
    struct lock frame_lock;
};

struct lock evict_lock;
void frame_table_init (void);
void add_frame (void*, struct spl_pe*);
void remove_frame (void*);
void* get_frame (struct spl_pe*);
struct frame* vm_get_fe (void*);
#endif