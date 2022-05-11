#include <list.h>
#include "threads/synch.h"
#include "threads/thread.h"

struct frame{
    void *frame;
    tid_t tid;
    void *upage;
    struct list_elem elem;
};

void frame_table_init (void);
void add_frame (void*, void*);
void remove_frame (void*);
void* get_frame (void*);