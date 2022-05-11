#include <list.h>
#include "threads/synch.h"
#include "threads/thread.h"

struct frame{
    void *frame;
    tid_t tid;
    struct list_elem elem;
};

void frame_table_init (void);
void vm_add_fe (void*);
void vm_remove_fe (void*);